# TPT multiplayer: native Windows build and Linux↔Windows determinism

**Engineering handoff, 2 November 2026.** Continues `SEAL_BATCHING_2026-11-02.md`.
Completes next step 1 for one platform pair: Windows x86-64 clients
simulating locally against a Linux x86-64 server.

## Summary

| Item | Status |
|---|---|
| Windows x86-64 `tpt-server.exe`, `tpt-remote.exe`, `tpt-replica.exe` (MinGW-w64 GCC 13, UCRT, static) | **Built** with `tools/multiplayer/build-windows-mingw.sh` |
| Native Windows unit tests (`multiplayer-tests`, `tpt-replica-tests`, `tpt-checkpoint-tests`) | **Pass** on Windows 11 |
| Linux server ↔ Windows local-physics replicas, live fingerprint comparison | **PASS**: 20 min, about 71k ticks, 1,180 fingerprints, 0 divergence |
| Linux suite after the build changes | 17 pass, 0 fail, 1 skip (unchanged) |
| MSVC build, ARM, macOS | Not done |

## Determinism result

20-minute run, 27 editing rounds, 271 edits, seed 4242
(`determinism-20min/summary.json` in the workspace):

| Replica | Platform / build | Final tick | Edits applied | Fingerprints verified | Divergences |
|---|---|---:|---:|---:|---:|
| LinuxReplica (control) | WSL Ubuntu, GCC 13.3 | 72,013 | 271 | 1,198 | 0 |
| WinReplica | Windows 11, MinGW GCC 13 / UCRT | 70,866 | 271 | 1,180 | 0 |
| WinLocal (`tpt-remote --local-sim`, paced) | Windows 11, MinGW GCC 13 / UCRT | 70,857 | 271 | 1,179 | 0 (0 resyncs) |

A 60 s smoke run beforehand also passed (61 fingerprints each, 14 edits).

**Why the Windows replicas end about 1,150 ticks lower:** the WSL clock runs
1.6% slow compared with Windows (measured ratio 0.9836 over 30 s). The server
stepped 60/s by its own clock, which is 58.9/s in Windows time. Each replica
stops after 1,200 s of its own clock, so the Windows ones simply stopped
earlier in server time. Their logs show steady stepping at the server's rate,
with no stalls and no backlog. This is an artifact of the test machine, but it
confirms that clients must follow the server's measured rate, not assume 60/s.

Step cost on the same CPU (`tpt-rollback-benchmark`): the Windows MinGW build
is **faster** than the Linux build under WSL, for example water-30k 4.6 vs
5.8 ms per step and dust-180k 17.9 vs 19.9 ms.

Method (`tools/multiplayer/cross_platform_determinism.py`, run from Windows):
the authoritative `tpt-server` (Linux build) runs in WSL with seed 4242. An
admin editor in WSL reconnects every 45 s and applies a new, reproducibly
random batch of edits. A round includes a metal tray, a wooden shelf, a sparked
metal wire, eight random strokes and boxes chosen from 22 reactive elements
(fire, lava, plasma, acid, gunpowder, nitro, plants, liquids, ice, salt and
others) and a heat-tool blast. Three replicas join 3 s in, through the
exact-checkpoint path, and must match every fingerprint (one per 60 ticks; each
covers all 14 state groups):

- `LinuxReplica`: Linux `tpt-replica`, the control;
- `WinReplica`: Windows `tpt-replica.exe` (immediate stepping);
- `WinLocal`: Windows `tpt-remote.exe --local-sim --headless`, using the paced
  stepping from the seal-batching change.

Any mismatch stops that replica, and the server log records the tick and the
differing groups. The world is not reset between rounds. No Newtonian gravity
or Lua (both unsupported for checkpoints).

What this does **not** cover: MSVC builds; ARM CPUs; other x86 CPUs (one
Windows 11 host, with WSL on the same CPU); gravity or Lua worlds; scenes
outside this element list (stickmen, portals, wifi, life and others).

## How the Windows build works

`tools/multiplayer/build-windows-mingw.sh` (Ubuntu or WSL, after
`sudo apt install g++-mingw-w64-x86-64`). Meson fetches the upstream
`tpt-libs-prebuilt-x86_64-windows-mingw-static-release` pack (24.6 MB,
SHA-256 pinned in its wrap file).

That pack is built by MSYS2 against the Universal CRT. Ubuntu's MinGW targets
msvcrt, so the script:

- links `-lucrt` in place of `-lmsvcrt` through a GCC specs file (GCC 13 has
  no `-mcrtdll`). The executables import only `api-ms-win-crt-*`, not
  `msvcrt.dll`, so math library calls go to the same UCRT as an official
  MSYS2 UCRT build;
- keeps msvcrt-style headers. `-D_UCRT` changes libstdc++'s mangled names
  (`mbstate_t`) and then fails to link against Ubuntu's libstdc++;
- uses GCC's win32 thread model. Ubuntu's winpthreads is built for msvcrt
  (`_setjmp`);
- adds a small shim for five symbols the prebuilt libraries expect from
  MSYS2's UCRT import libraries: `_setjmp` (tail jump to UCRT's
  `__intrinsic_setjmp`), two UCRT header inlines, and `fseeko64`/`ftello64`
  (forwarded to `_fseeki64`/`_ftelli64`).

Two source changes were needed:

- `meson.build`: `_WIN32_WINNT` is now 0x0601 (Windows 7) except for the
  32-bit XP target, which keeps 0x0501. GCC's win32 thread model only provides
  `std::condition_variable` from 0x0600.
- `src/multiplayer/meson.build`: Windows multiplayer executables reserve a
  16 MiB stack (`-Wl,--stack` / `/STACK`). With the 1 MiB default, the core
  tests overflowed the stack (`0xC00000FD`) because simulation objects live on
  the stack. Linux defaults to 8 MiB. `std::thread` threads inherit this size.

Rebuilding from the script gives executables identical in size to the ones
tested, differing only in the PE timestamp.

Practical note: Windows clients reach a server in WSL at `ws://127.0.0.1:PORT`
through WSL's localhost forwarding.

## Running it

```sh
# WSL / Linux, from the source root
meson setup build-mp --buildtype=release -Dstatic=prebuilt -Dlua=none -Dhttp=false -Dbuild_multiplayer=true -Dresolve_vcs_tag=no
ninja -C build-mp
tools/multiplayer/build-windows-mingw.sh build-win
```

```bat
rem Windows, after copying build-win\src\multiplayer\*.exe to a folder
set TPT_ROOM_TOKEN=room password
tpt-remote.exe --url ws://SERVER:34403 --name Alice --local-sim
python tools\multiplayer\cross_platform_determinism.py --seconds 1200 --out determinism-run
```

## Next steps

1. MSVC build (needs the Visual Studio C++ workload) and the same comparison:
   a different compiler and a different libm-equivalent path.
2. ARM64 (Windows on ARM or Linux aarch64) and a second x86 vendor.
3. Add stickmen, portals, wifi, life and pressure-driven scenes to the
   harness, and a Windows-hosted server with Linux replicas.
4. Then decide whether `--local-sim` can become the default for x86-64.
