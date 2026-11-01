"""Native full-featured TPT + multiplayer build on Windows, Linux and macOS."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-native")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--static", choices=("prebuilt", "none", "system"), default="prebuilt")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    repo = Path(__file__).resolve().parents[2]
    build = (repo/args.build_dir).resolve()
    meson = [sys.executable, "-m", "mesonbuild.mesonmain"]
    setup = meson+["setup", str(build), str(repo), "--buildtype=release", "-Dbuild_multiplayer=true",
                   "-Dstatic="+args.static, "-Dlua=auto", "-Dhttp=true", "-Dresolve_vcs_tag=no"]
    if sys.platform == "win32":
        setup.append("--vsenv")
    if (build/"meson-private/coredata.dat").exists():
        setup.append("--reconfigure")
    try:
        subprocess.run(setup, cwd=repo, check=True)
    except subprocess.CalledProcessError:
        if sys.platform == "win32":
            print("Windows builds require Visual Studio's Desktop development with C++ workload (MSVC and Windows SDK). See meson-logs/meson-log.txt.", file=sys.stderr)
        raise
    subprocess.run(meson+["compile", "-C", str(build), "-j", str(args.jobs)], cwd=repo, check=True)
    if args.test:
        subprocess.run(meson+["test", "-C", str(build), "--print-errorlogs"], cwd=repo, check=True)


if __name__ == "__main__":
    main()
