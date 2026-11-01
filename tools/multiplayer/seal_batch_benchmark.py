#!/usr/bin/env python3
"""Compare local-sim receive bandwidth for several --seal-batch-ms settings.

Usage: seal_batch_benchmark.py TPT_SERVER TPT_REMOTE [--seconds 30] [--batches 0,50,100,250]

Each run starts a fresh server, has a video admin client pour water and dust
into a metal tray, then measures a headless --local-sim watcher over the last
half of the run. Reports application payload only (loopback, no TLS).
"""
import argparse, json, os, socket, statistics, subprocess, sys, tempfile, time
from pathlib import Path


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(server_bin, client_bin, batch, seconds):
    with tempfile.TemporaryDirectory(prefix="tpt-seal-bench-") as td:
        root = Path(td)
        port = free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="bench", TPT_ADMIN_TOKEN="bench-admin", SDL_VIDEODRIVER="dummy")
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([server_bin, "--port", str(port), "--seed", "4242", "--seal-batch-ms", str(batch)],
                                  cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                if time.monotonic() > deadline or server.poll() is not None:
                    raise RuntimeError("server did not start")
                time.sleep(.1)
            script = root / "scene.jsonl"
            script.write_text("".join(json.dumps(c) + "\n" for c in [
                dict(op="brush", x1=100, y1=330, x2=500, y2=330, radius=3, element=5),   # metal tray
                dict(op="brush", x1=100, y1=250, x2=100, y2=330, radius=3, element=5),
                dict(op="brush", x1=500, y1=250, x2=500, y2=330, radius=3, element=5),
                dict(op="brush", x1=150, y1=120, x2=450, y2=120, radius=8, element=2),   # water
                dict(op="brush", x1=150, y1=160, x2=450, y2=160, radius=6, element=1),   # dust
            ]))
            editor_env = dict(env)
            editor = subprocess.Popen([client_bin, "--url", f"ws://127.0.0.1:{port}", "--name", "Editor", "--headless",
                                       "--run-for", "4", "--no-reconnect", "--commands", str(script)],
                                      cwd=root, env=editor_env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
            watcher_env = dict(env); watcher_env.pop("TPT_ADMIN_TOKEN")
            metrics = root / "watcher.jsonl"
            # The measured player also edits: one small dot about every 0.7 s, for own-edit latency.
            dots = root / "dots.jsonl"
            dots.write_text("".join(json.dumps(dict(op="brush", x1=60 + 9 * i % 480, y1=60 + 7 * i % 60,
                                                    x2=60 + 9 * i % 480, y2=60 + 7 * i % 60, radius=1, element=1)) + "\n"
                                    for i in range(int(seconds / .7) - 4)))
            watcher = subprocess.run([client_bin, "--url", f"ws://127.0.0.1:{port}", "--name", "Watcher", "--headless",
                                      "--local-sim", "--run-for", str(seconds), "--no-reconnect", "--metrics", str(metrics),
                                      "--commands", str(dots)],
                                     cwd=root, env=watcher_env, capture_output=True, text=True, timeout=seconds + 30)
            editor.wait(timeout=30)
            if watcher.returncode:
                raise RuntimeError(watcher.stdout + watcher.stderr)
            rows = [json.loads(l) for l in metrics.read_text().splitlines() if l.strip()]
            tail = rows[len(rows) // 2:]
            ticks = tail[-1]["localTick"] - tail[0]["localTick"]
            latency = sorted(ms for r in rows for ms in r.get("editLatencyMs", []))
            pick = lambda q: latency[min(len(latency) - 1, int(q * len(latency)))] if latency else float("nan")
            return dict(batch=batch, edits=len(latency), latencyP50=pick(.5), latencyP95=pick(.95),
                        latencyMax=latency[-1] if latency else float("nan"), lead=tail[-1].get("paceLeadTicks"),
                        receiveMbps=statistics.median(r["receiveMbps"] for r in tail),
                        sealsPerTick=(tail[-1]["seals"] - tail[0]["seals"]) / max(ticks, 1),
                        ticksPerSecond=ticks / max(len(tail) - 1, 1),
                        verified=tail[-1]["verifiedHashes"], resyncs=tail[-1]["resyncs"])
        finally:
            server.terminate(); server.wait(timeout=10); log.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("server"); p.add_argument("client")
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--batches", default="0,50,100,250")
    a = p.parse_args()
    print("batch_ms,median_receive_mbps,seals_per_tick,ticks_per_s,verified_hashes,resyncs,"
          "own_edits,edit_latency_p50_ms,edit_latency_p95_ms,edit_latency_max_ms,pace_lead_ticks")
    server, client = str(Path(a.server).resolve()), str(Path(a.client).resolve())
    for b in [int(x) for x in a.batches.split(",")]:
        r = run(server, client, b, a.seconds)
        print(f"{r['batch']},{r['receiveMbps']:.4f},{r['sealsPerTick']:.3f},{r['ticksPerSecond']:.1f},{r['verified']},{r['resyncs']},"
              f"{r['edits']},{r['latencyP50']:.1f},{r['latencyP95']:.1f},{r['latencyMax']:.1f},{r['lead']}",
              flush=True)


if __name__ == "__main__":
    sys.exit(main())
