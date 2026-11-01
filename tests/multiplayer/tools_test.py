"""Exercise the extended tool surface through the real native command-line client."""
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import zlib

fixture = runpy.run_path(str(Path(__file__).with_name("network_test.py")))
client = fixture["CLIENT"]
catalog = json.loads(subprocess.check_output([client, "--list-tools"], text=True))
identifiers = {t["identifier"] for t in catalog}
assert len(identifiers) > 200 and {"DEFAULT_PT_METL", "DEFAULT_TOOL_HEAT", "DEFAULT_WL_FAN"} <= identifiers
assert subprocess.run([client, "--tool", "invalid"], capture_output=True).returncode != 0
with tempfile.TemporaryDirectory(prefix="tpt-tools-") as tmp:
    folder = Path(tmp)
    server, port, log, env = fixture["start"](folder)
    viewer = None
    try:
        viewer = fixture["WS"](port); viewer.hello(); viewer.frame()
        def tool(identifier, action, **fields):
            return dict(op="tool", tool=identifier, action=action, x1=100, y1=100, radius=2, **fields)
        commands = [tool("DEFAULT_PT_METL", "box", x2=130, y2=130),
                    tool("DEFAULT_TOOL_HEAT", "point"),
                    tool("DEFAULT_WL_FAN", "point"),
                    tool("DEFAULT_WL_FAN", "line", x2=120, y2=110, dragging=False),
                    tool("DEFAULT_PT_WATR", "line", x2=120, y2=100, flags=1, replace=14)]
        path = folder/"commands.jsonl"
        path.write_text("\n".join(map(json.dumps, commands))+"\n")
        result = subprocess.run([client, "--url", f"ws://127.0.0.1:{port}", "--headless", "--run-for", "7",
                                 "--no-reconnect", "--commands", str(path), "--metrics", str(folder/"metrics.jsonl")],
                                env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stdout+result.stderr
        final = json.loads((folder/"metrics.jsonl").read_text().splitlines()[-1])
        assert final["revision"] == len(commands) and final["pendingEdits"] == 0
        shared = viewer.frame(revision=len(commands))
        assert zlib.crc32(shared["pixels"]) == final["canvasChecksum"]
        path.write_text(json.dumps(tool("UNKNOWN", "point"))+"\n")
        rejected = subprocess.run([client, "--url", f"ws://127.0.0.1:{port}", "--headless", "--run-for", "3",
                                   "--no-reconnect", "--commands", str(path)],
                                  env=env, capture_output=True, text=True, timeout=10)
        assert rejected.returncode != 0 and "script command rejected" in rejected.stderr
        print("PASS: complete advertised tool catalog, native CLI commands, shapes/walls/heat/replace, convergence and command rejection")
    finally:
        if viewer:
            viewer.close()
        fixture["stop"](server, log)
