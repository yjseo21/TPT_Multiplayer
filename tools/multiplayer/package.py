"""Package a built TPT Multiplayer for release: one zip per platform.

The zip holds the game (powder), the dedicated server (tpt-server), the
command-line client (tpt-remote), the multiplayer guide and the licence.
Executable permissions are kept, so the zip works as is on Linux and macOS.
Linux and macOS executables are stripped of their symbol tables (copies only).

Usage: python tools/multiplayer/package.py --build-dir build-native --name linux-x86_64 --out dist
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-native")
    parser.add_argument("--name", required=True, help="platform label in the file name, for example windows-x86_64")
    parser.add_argument("--version", default="", help="release version in the file name, for example 1.0.0")
    parser.add_argument("--out", default="dist")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    build = (repo / args.build_dir).resolve()
    exe = ".exe" if sys.platform == "win32" else ""
    files = {
        f"powder{exe}": build / f"powder{exe}",
        f"tpt-server{exe}": build / "src" / "multiplayer" / f"tpt-server{exe}",
        f"tpt-remote{exe}": build / "src" / "multiplayer" / f"tpt-remote{exe}",
        "README.multiplayer.md": repo / "README.multiplayer.md",
        "HOSTING_SECURITY.md": repo / "docs" / "multiplayer" / "HOSTING_SECURITY.md",
        "LICENSE": repo / "LICENSE",
    }
    if sys.platform.startswith("linux"):
        files["tpt-server.service"] = repo / "packaging" / "multiplayer" / "tpt-server.service"
        files["tpt-server@.service"] = repo / "packaging" / "multiplayer" / "tpt-server@.service"  # per-user rooms
    missing = [str(path) for path in files.values() if not path.is_file()]
    if missing:
        sys.exit("missing build outputs: " + ", ".join(missing))
    out = (repo / args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    stem = "TPT-Multiplayer" + (f"-{args.version}" if args.version else "") + f"-{args.name}"
    archive = out / f"{stem}.zip"
    staging = Path(tempfile.mkdtemp(prefix="tpt-package-"))
    strip = shutil.which("strip") if sys.platform != "win32" else None
    for name, path in list(files.items()):
        if strip and not name.endswith((".md", "LICENSE", ".service")):
            copy = staging / name
            shutil.copy2(path, copy)
            subprocess.run([strip, "-S" if sys.platform == "darwin" else "--strip-unneeded", str(copy)], check=True)
            files[name] = copy
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, path in files.items():
            info = zipfile.ZipInfo.from_file(path, f"{stem}/{name}")  # keeps the mode bits
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as data:
                z.writestr(info, data.read())
    shutil.rmtree(staging, ignore_errors=True)
    print(archive)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as output:
            output.write(f"archive={archive}\n")


if __name__ == "__main__":
    main()
