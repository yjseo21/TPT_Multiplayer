"""Joining a busy room over a distant, slow link.

Usage: python3 tests/multiplayer/distant_join_test.py SERVER CLIENT

A builder fills the room (about 1.5-2 MB as a snapshot). A player then joins
through a proxy that adds 50 ms each way (100 ms round trip) and caps the link
at 150 KB/s, so the snapshot takes over ten seconds to arrive while the room
keeps running. The player must show download progress, catch up to the room's
present without jumping ahead or resynchronizing, and then verify fingerprints
like any other player.
"""
import asyncio
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

SERVER, CLIENT = (str(Path(p).resolve()) for p in sys.argv[1:3])
DELAY, RATE = 0.05, 150 * 1024


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def proxy(listen, target, ready):
    """Delays every byte by DELAY each way, in order, at most RATE bytes a second."""
    async def pipe(reader, writer):
        queue = asyncio.Queue()

        async def receive():
            while data := await reader.read(16384):
                await queue.put((time.monotonic() + DELAY, data))
            await queue.put((None, None))

        async def send():
            free = time.monotonic()
            while True:
                due, data = await queue.get()
                if data is None:
                    break
                await asyncio.sleep(max(0, due - time.monotonic()))
                free = max(free, time.monotonic()) + len(data) / RATE
                await asyncio.sleep(max(0, free - time.monotonic()))
                writer.write(data)
                await writer.drain()
            writer.close()

        await asyncio.gather(receive(), send(), return_exceptions=True)

    async def handle(client_reader, client_writer):
        server_reader, server_writer = await asyncio.open_connection("127.0.0.1", target)
        await asyncio.gather(pipe(client_reader, server_writer), pipe(server_reader, client_writer), return_exceptions=True)

    async def main():
        server = await asyncio.start_server(handle, "127.0.0.1", listen)
        ready.set()
        async with server:
            await server.serve_forever()

    asyncio.run(main())


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def main():
    with tempfile.TemporaryDirectory(prefix="tpt-distant-join-") as td:
        root = Path(td)
        port, far = free_port(), free_port()
        env = dict(os.environ, TPT_ROOM_TOKEN="room-test", SDL_VIDEODRIVER="dummy")
        env.pop("TPT_ADMIN_TOKEN", None)
        log = (root / "server.log").open("w+")
        server = subprocess.Popen([SERVER, "--port", str(port), "--seed", "97", "--run-for", "80"], cwd=root, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while "Listening on" not in (root / "server.log").read_text():
                assert time.monotonic() < deadline and server.poll() is None, (root / "server.log").read_text()
                time.sleep(.1)
            ready = threading.Event()
            threading.Thread(target=proxy, args=(far, port, ready), daemon=True).start()
            assert ready.wait(5)

            # A busy world: bands of dust, water, oil and stone across the canvas.
            lines = [dict(op="brush", x1=10, y1=y, x2=600, y2=y, radius=3, element=[1, 2, 3, 5][i % 4])
                     for i, y in enumerate(range(30, 370, 17))]
            (root / "build.jsonl").write_text("".join(json.dumps(line) + "\n" for line in lines))
            builder = subprocess.run([CLIENT, "--url", f"ws://127.0.0.1:{port}", "--name", "Builder", "--headless",
                                      "--run-for", "6", "--no-reconnect", "--commands", str(root / "build.jsonl")],
                                     cwd=root, env=env, capture_output=True, text=True, timeout=30)
            assert builder.returncode == 0, builder.stdout + builder.stderr

            distant = subprocess.run([CLIENT, "--url", f"ws://127.0.0.1:{far}", "--name", "Distant", "--headless",
                                      "--local-sim", "--run-for", "45", "--no-reconnect",
                                      "--metrics", str(root / "distant.jsonl")],
                                     cwd=root, env=env, capture_output=True, text=True, timeout=75)
            assert distant.returncode == 0, distant.stdout + distant.stderr
            samples = rows(root / "distant.jsonl")
            downloading = [r for r in samples if r.get("downloadBytes")]
            assert downloading, "no download progress was reported"
            total = downloading[-1]["downloadBytes"]
            assert total > 500_000, total  # big enough that the link, not the CPU, sets the join time
            assert any(r["downloadedBytes"] < total // 2 for r in downloading), "progress was not reported during the download"
            final = samples[-1]
            assert final["status"] == "LOCAL SIM", final
            assert final["skipAheads"] == 0 and final["resyncs"] == 0 and final["connections"] == 1, final
            assert final["verifiedHashes"] >= 10, final
            assert final["revision"] == len(lines), final  # every edit arrived inside the snapshot
        finally:
            server.terminate(); server.wait(timeout=10); log.close()
        assert "diverged" not in (root / "server.log").read_text()
    seconds = len(downloading)
    print(f"PASS: a {total / 1e6:.1f} MB room joined over a 100 ms, 150 KB/s link (download about {seconds} s), "
          f"caught up without jumping ahead and verified {final['verifiedHashes']} fingerprints")


if __name__ == "__main__":
    main()
