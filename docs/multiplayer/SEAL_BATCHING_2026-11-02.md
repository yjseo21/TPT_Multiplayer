# TPT multiplayer: batched tick seals and paced local stepping

> **Superseded default:** `LATENCY_2026-11-02.md` measured that 100 ms batching adds about 150 ms to every own edit. The default is now `--seal-batch-ms 0`. Batching remains available, and the pacing below is now adaptive.

**Engineering handoff, 2 November 2026 (later the same day).** This continues
`LOCAL_CLIENT_2026-11-02.md` and completes its next step 2. The wire format is
unchanged: the server sends fewer `tick_seal` messages.

## Summary

| Item | Status |
|---|---|
| Server batches empty-interval seals (`--seal-batch-ms`, default 100) | **Done** |
| `tpt-remote --local-sim` steps sealed ticks at the server's pace (no visible jumps) | **Done** |
| Unit test for paced `ReplicaState`; seal-rate assertion in `multiplayer-local-client` | **Done**, passing |
| Full suite on this host (WSL Ubuntu 24.04, GCC 13.3, release) | **17 pass, 0 fail, 1 skip** (impaired-network: `tc` needs root) |
| Native Windows build | **Blocked**: no C++ compiler on the Windows host (see below) |

## Measurement

`tools/multiplayer/seal_batch_benchmark.py`: a settled water-and-dust tray, one
headless `--local-sim` watcher, median over the second half of a 30 s run.
Loopback, application payload only.

| `--seal-batch-ms` | Median receive | Seals per physics tick | Steps/s | Verified hashes | Resyncs |
|---:|---:|---:|---:|---:|---:|
| 0 (previous behavior) | 0.048 Mbps | 1.000 | 59.6 | 30 | 0 |
| 50 | 0.022 Mbps | 0.288 | 59.6 | 30 | 0 |
| **100 (default)** | **0.017 Mbps** | 0.168 | 59.9 | 30 | 0 |
| 250 | 0.013 Mbps | 0.083 | 59.1 | 30 | 0 |

At the default, seal traffic falls by about 64%. The rest is mostly presence
and telemetry, which batching does not touch. Fingerprint checks are unchanged:
seals that carry a digest are never merged away.

```sh
python3 tools/multiplayer/seal_batch_benchmark.py build-mp/src/multiplayer/tpt-server build-mp/src/multiplayer/tpt-remote
```

## Server rules (`Server.cpp`, `Peer::Text`)

A local-sim peer holds at most one pending seal. Later seals replace it, which
is safe because a seal lets the client run every tick up to it. The pending seal
is sent:

- before any other message to that peer, so a `command_commit` never overtakes
  the seal that brings the client to its tick;
- immediately, if the new seal carries a digest (every 60 ticks, and on paused
  edits) or reports `paused`;
- before a checkpoint is queued;
- by the sender thread once it is `sealBatch` seconds old.

The older backlog coalescing (merging adjacent queued seals when a sender falls
behind) now skips seals with a digest, so a slow peer still verifies them.
`--seal-batch-ms 0` restores one seal per tick. The welcome message reports
`sealBatchMs` to local-sim peers. Video peers are unaffected.

## Client pacing (`ReplicaState`, `Client.cpp`)

`ReplicaState::SetPaced(true)` makes `OnJson` validate and queue commits and
seals instead of applying them. `Pump(maxTicks)` applies the queue in wire
order. It runs at most `maxTicks` physics steps and finishes a seal, including
its fingerprint check, only once the seal's tick is reached. Commits and
zero-step seals (paused edits) apply without using the budget. The queue is
bounded at 8192 messages.

Each loop iteration, `tpt-remote --local-sim` earns step credit at the server's
observed seal rate. It measures that rate over 1 s windows and keeps the old
estimate while the world is paused. If the backlog grows past two batches plus 2
ticks, it catches up to one batch. Local display therefore runs about one batch
(about 100 ms) behind the newest seal. That latency is the cost of the bandwidth
saving. Edits still arrive without delay, because a commit flushes the pending
seal. The debug corruption hook now runs after pumping.

`tpt-replica` keeps the immediate (unpaced) mode. Its `--min-seals` counts
messages, so `multiplayer-local-replica-*` now take about 10 s instead of 3–4 s
because seals arrive about 6× less often. Both are well inside their 95 s timeouts.

## Building on this Windows host

Build and tests above ran in WSL Ubuntu from a copy at `~/tpt-mp/src`. The
Windows-side source here is the master copy; the WSL copy is synced from it.
Meson came from `.transfer/meson.tgz` through a small `~/bin/meson` wrapper.
One fix was needed: the extracted
`subprojects/tpt-libs-prebuilt-x86_64-linux-gnu-static-release-*` folder was
incomplete (no `lib/`). Deleting it so Meson re-extracts it from
`subprojects/packagecache` fixes the build.

Native Windows is still not done. Visual Studio 18 Community and VS 2022 Build
Tools are installed, but neither has the "Desktop development with C++"
workload (`VC/Tools/MSVC` is absent), and there is no MinGW. WSL `sudo` needs a
password, so `mingw-w64` cannot be installed for cross-compiling either. The
cross-platform determinism test (next step 1 in the previous doc) needs one of
these:

1. Install the MSVC C++ workload, then build with the upstream
   `.github/msvca64-ghactions.ini`-style configuration and the
   `x86_64-windows-msvc-static-release` prebuilt wrap, or
2. `sudo apt install g++-mingw-w64-x86-64` in WSL and cross-compile with the
   `x86_64-windows-mingw-static-release` wrap.

Then run a Windows `tpt-remote --local-sim` against the Linux `tpt-server` in
WSL for 10k–100k ticks and compare fingerprints.

## Next steps

1. Windows toolchain (above), then cross-platform fingerprint qualification.
2. Join cost: cache and zstd-compress checkpoints.
3. Long WAN soak with `--local-sim` under netem (needs root for `tc`).
4. Optional: fold presence/telemetry into fewer messages for local-sim peers;
   that is now most of the remaining ~17 kbps.
