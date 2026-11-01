#!/usr/bin/env python3
"""Cross-platform local-physics determinism run: Linux server, Windows replicas.

Run from Windows with Python 3. The authoritative tpt-server and a scene editor
run inside WSL (Linux build). Three replicas join and verify every fingerprint
(one per 60 ticks) for the whole run:

  LinuxReplica  tpt-replica in WSL (control: same binary family as the server)
  WinReplica    tpt-replica.exe on Windows (immediate stepping)
  WinLocal      tpt-remote.exe --local-sim --headless on Windows (paced stepping)

Any fingerprint mismatch stops that replica with an error, and the server log
names the tick and differing state groups. The editor reconnects every
--round-seconds with a new batch of reactive edits (fire, lava, explosives,
electronics, liquids, plants, heat), so the world stays busy.

Usage (paths default to this workspace's layout):
  python cross_platform_determinism.py --seconds 900 --out results-dir
"""
import argparse, json, os, random, re, socket, subprocess, sys, time
from pathlib import Path

WSL = ["wsl.exe", "-d", "Ubuntu", "--"]
ROOM, ADMIN = "determinism", "determinism-admin"
W, H = 612, 384

ELEMENTS = ["DUST", "WATR", "OIL", "FIRE", "LAVA", "PLNT", "SALT", "ICEI", "SAND", "GUNP", "NITR",
            "WOOD", "COAL", "SNOW", "STNE", "ACID", "PLSM", "WTRV", "GLAS", "BRCK", "MERC", "SLTW"]


def scene(round_index):
    """Deterministic, varied batch of edits for one editor round."""
    rng = random.Random(1000 + round_index)
    tool = lambda ident, action, **kw: dict(op="tool", tool=ident, action=action, brush="ellipse",
                                            dragging=False, **kw)
    cmds = []
    if round_index == 0:
        # A metal tray, a wooden shelf and a sparked metal wire with an insulator gap.
        cmds += [tool("DEFAULT_PT_METL", "box", x1=40, y1=350, x2=570, y2=356, radius=0),
                 tool("DEFAULT_PT_METL", "box", x1=40, y1=250, x2=46, y2=356, radius=0),
                 tool("DEFAULT_PT_METL", "box", x1=564, y1=250, x2=570, y2=356, radius=0),
                 tool("DEFAULT_PT_WOOD", "line", x1=120, y1=200, x2=260, y2=200, radius=2),
                 tool("DEFAULT_PT_METL", "line", x1=300, y1=60, x2=560, y2=60, radius=0),
                 tool("DEFAULT_PT_SPRK", "point", x1=300, y1=60, radius=0)]
    for _ in range(8):
        e = rng.choice(ELEMENTS)
        x1, y1 = rng.randrange(60, W - 60), rng.randrange(40, 300)
        x2, y2 = min(W - 50, x1 + rng.randrange(10, 120)), min(330, y1 + rng.randrange(0, 30))
        cmds.append(tool(f"DEFAULT_PT_{e}", rng.choice(["line", "box", "point"]),
                         x1=x1, y1=y1, x2=x2, y2=y2, radius=rng.randrange(1, 7)))
    cmds.append(tool("DEFAULT_TOOL_HEAT", "point", x1=rng.randrange(60, W - 60), y1=rng.randrange(200, 340), radius=8))
    cmds.append(tool("DEFAULT_PT_SPRK", "point", x1=300, y1=60, radius=0))
    return cmds


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wsl_env(*pairs):
    return ["env", *pairs]


def main():
    here = Path(__file__).resolve()
    a = argparse.ArgumentParser()
    a.add_argument("--seconds", type=int, default=300)
    a.add_argument("--round-seconds", type=int, default=45)
    a.add_argument("--linux-bin", default="/home/black/tpt-mp/src/build-mp/src/multiplayer")
    a.add_argument("--win-bin", default=str(here.parents[5] / "win-bin"))
    a.add_argument("--out", default="determinism-run")
    o = a.parse_args()
    out = Path(o.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    win = Path(o.win_bin)
    port = free_port()
    # Backslashes do not survive wsl.exe argument passing; forward slashes do.
    linux_out = subprocess.run(WSL + ["wslpath", "-a", out.as_posix()], capture_output=True, text=True).stdout.strip()
    if not linux_out: raise SystemExit("cannot map output directory into WSL")
    total = o.seconds
    server_log = (out / "server.log").open("w")
    server = subprocess.Popen(WSL + wsl_env(f"TPT_ROOM_TOKEN={ROOM}", f"TPT_ADMIN_TOKEN={ADMIN}") +
                              [f"{o.linux_bin}/tpt-server", "--port", str(port), "--seed", "4242",
                               "--run-for", str(total + 60)], stdout=server_log, stderr=subprocess.STDOUT)
    procs = {}
    try:
        deadline = time.monotonic() + 20
        while "Listening on" not in (out / "server.log").read_text(errors="replace"):
            if time.monotonic() > deadline or server.poll() is not None:
                raise SystemExit("server did not start:\n" + (out / "server.log").read_text(errors="replace"))
            time.sleep(.2)

        def editor(round_index):
            script = out / f"scene{round_index}.jsonl"
            script.write_text("".join(json.dumps(c) + "\n" for c in scene(round_index)))
            log = (out / f"editor{round_index}.log").open("w")
            return subprocess.Popen(WSL + wsl_env(f"TPT_ROOM_TOKEN={ROOM}", f"TPT_ADMIN_TOKEN={ADMIN}", "SDL_VIDEODRIVER=dummy") +
                                    [f"{o.linux_bin}/tpt-remote", "--url", f"ws://127.0.0.1:{port}", "--name", "Editor",
                                     "--headless", "--no-reconnect", "--run-for", "15",
                                     "--commands", f"{linux_out}/scene{round_index}.jsonl"],
                                    stdout=log, stderr=subprocess.STDOUT)

        editors = [editor(0)]
        time.sleep(3)  # join with a non-trivial world, exercising the checkpoint path
        start = time.monotonic()
        run_for = str(total)
        env = dict(os.environ, TPT_ROOM_TOKEN=ROOM, SDL_VIDEODRIVER="dummy")
        env.pop("TPT_ADMIN_TOKEN", None)
        url = f"ws://127.0.0.1:{port}"
        procs["LinuxReplica"] = subprocess.Popen(
            WSL + wsl_env(f"TPT_ROOM_TOKEN={ROOM}") + [f"{o.linux_bin}/tpt-replica", "--url", url, "--name", "LinuxReplica",
                                                       "--run-for", run_for, "--output", f"{linux_out}/linux-replica.png"],
            stdout=(out / "LinuxReplica.log").open("w"), stderr=subprocess.STDOUT)
        procs["WinReplica"] = subprocess.Popen(
            [str(win / "tpt-replica.exe"), "--url", url, "--name", "WinReplica", "--run-for", run_for,
             "--output", str(out / "win-replica.png")], cwd=out, env=env,
            stdout=(out / "WinReplica.log").open("w"), stderr=subprocess.STDOUT)
        procs["WinLocal"] = subprocess.Popen(
            [str(win / "tpt-remote.exe"), "--url", url, "--name", "WinLocal", "--headless", "--local-sim",
             "--no-reconnect", "--run-for", run_for, "--metrics", str(out / "win-local.jsonl")], cwd=out, env=env,
            stdout=(out / "WinLocal.log").open("w"), stderr=subprocess.STDOUT)
        next_round, round_index = start + o.round_seconds, 1
        while time.monotonic() - start < total + 20 and any(p.poll() is None for p in procs.values()):
            if time.monotonic() >= next_round and time.monotonic() - start < total - 20:
                editors.append(editor(round_index)); round_index += 1; next_round += o.round_seconds
            dead = [n for n, p in procs.items() if p.poll() not in (None, 0)]
            if dead:
                print("replica stopped early:", dead, flush=True)
                break
            time.sleep(1)
        for p in procs.values():
            try: p.wait(timeout=60)
            except subprocess.TimeoutExpired: p.kill()
        for e in editors:
            try: e.wait(timeout=30)
            except subprocess.TimeoutExpired: e.kill()
    finally:
        for p in procs.values():
            if p.poll() is None: p.kill()
        server.terminate()
        try: server.wait(timeout=10)
        except subprocess.TimeoutExpired: server.kill()
        subprocess.run(WSL + ["pkill", "-f", f"--port {port}"], capture_output=True)
        server_log.close()

    summary = {"seconds": total, "editorRounds": len(editors),
               "editorFailures": sum(1 for e in editors if e.returncode not in (0, None))}
    for name in ("LinuxReplica", "WinReplica"):
        text = (out / f"{name}.log").read_text(errors="replace")
        m = re.search(r"replica OK tick=(\d+) revision=(\d+) seals=(\d+) edits=(\d+) matching_hashes=(\d+)", text)
        summary[name] = ({"ok": True, "tick": int(m[1]), "edits": int(m[4]), "verifiedHashes": int(m[5])} if m
                         else {"ok": False, "exit": procs[name].returncode, "log": text.strip().splitlines()[-3:]})
    rows = [json.loads(l) for l in (out / "win-local.jsonl").read_text().splitlines() if l.strip()] \
        if (out / "win-local.jsonl").exists() else []
    last = rows[-1] if rows else {}
    summary["WinLocal"] = {"ok": procs["WinLocal"].returncode == 0 and last.get("resyncs") == 0,
                           "exit": procs["WinLocal"].returncode, "localTick": last.get("localTick"),
                           "verifiedHashes": last.get("verifiedHashes"), "appliedCommands": last.get("appliedCommands"),
                           "resyncs": last.get("resyncs")}
    server_text = (out / "server.log").read_text(errors="replace")
    summary["serverDivergenceReports"] = [l for l in server_text.splitlines() if "diverged" in l]
    # No image comparison: replicas stop at different ticks of a running world,
    # and fire glow uses renderer-local randomness. Fingerprints are the criterion.
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    ok = all(summary[n]["ok"] for n in ("LinuxReplica", "WinReplica", "WinLocal")) and not summary["serverDivergenceReports"]
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
