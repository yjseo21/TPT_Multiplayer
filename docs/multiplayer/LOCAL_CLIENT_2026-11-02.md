# TPT multiplayer: playable local-physics client and automatic resync

**Engineering handoff, 2 November 2026.** Supersedes the status sections of
`LOCAL_PHYSICS_REPLICA_2026-10-31.md`; its protocol description still applies.

## Summary

| Milestone | Status |
|---|---|
| Official Meson build of the continuation source (server, video client, replica, tests) | **Done**, GCC 13.3 release build, Ubuntu 24.04 |
| Full Meson test suite | **16 pass, 0 fail**; impaired-network skipped (no `tc` on the build host) |
| `tpt-remote --local-sim`: interactive SDL client that simulates and renders locally | **Done**, tested |
| Automatic divergence detection, server-side diagnosis and resynchronization | **Done**, tested with injected corruption |
| Cross-OS / cross-CPU / cross-compiler determinism | Not tested (all runs: one Linux x86-64 GCC build) |
| Long WAN sessions, Windows build, seal batching | Not done |

## 1. Meson build: what was broken and fixed

The 31 October handoff could not run Meson. Running it showed 11 of 17 suites
failing. None of the failures were engine bugs; they were test-harness defects
that a non-Meson build had hidden:

1. **Asserts compiled out.** The project sets `b_ndebug=true`, so `assert()`
   is a no-op in every Meson build. The standalone tests
   (`checkpoint_wire`, `timeline`, `command_record`, `import_seed`) put the code
   under test *inside* `assert(...)`, so in release builds it never ran.
   `multiplayer-timeline` "passed" while checking nothing; `checkpoint-wire`
   failed because skipped `Accept()` calls changed the sequence. Fix: those
   files now `#undef NDEBUG` before including `<cassert>`.
2. **Stale build ID in Python tests.** `Protocol.h` was bumped to
   `tpt-c205becb-mp3-seedfix1` but `network_test.py` hard-coded
   `tpt-c205becb-mp3`, so every socket test was rejected at `hello`. The tests
   now read `BuildId` from `src/multiplayer/Protocol.h`.
3. **Ill-posed tamper test.** `CheckpointTests` flipped one compressed byte and
   required rejection. That flip decoded to the *identical* world: deflate can
   encode the same bytes two ways (e.g. back-reference distance 1 vs 65 inside a
   run of zeros), and both zlib's Adler-32 and our CRC-32 correctly matched. The
   test now requires that every sampled compressed-byte flip is either rejected
   or restores the exact original state. It separately checks that a changed
   payload with the old CRC and a changed CRC are both rejected.
4. **Brush round-trip comparison.** jsoncpp treats `Int(1)` and `UInt64(1)` as
   unequal. The test now compares the wire form.
5. **Autosave race in `recovery_test.py`** (intermittent). The test recorded
   the newest save name *after* editing a paused world. If the edited world had
   already been saved, no newer save ever followed. The server log now prints
   `Autosave committed generation=N revision=R`, and the test waits for the
   revision that contains its edit.

## 2. Playing with local physics

```sh
export TPT_ROOM_TOKEN='room password'
./build-mp/src/multiplayer/tpt-server --port 34403
# each player:
./build-mp/src/multiplayer/tpt-remote --url ws://127.0.0.1:34403 --name Alice --local-sim
```

`--local-sim` keeps the whole `tpt-remote` interface: materials 1–9, the
E tool search, line, box and fill, brush shape, cursors, the roster, admin
pause, step, clear, lock and kick, F1/F2/F3 views, the F4 panel and automatic
reconnect. The picture comes from a local copy of the world:

- On join the server sends one exact checkpoint. After that it sends only
  ordered `command_commit` edits and `tick_seal` messages. The client steps its
  own engine to each seal and renders locally, at most 60 times per second and
  only when the tick, revision or view changed. No video frames are sent.
- Views are local. F1/F2/F3 re-render immediately and send nothing to the
  server.
- Editing is enabled only while the local world is synchronized. Edits still
  go to the server; the client sees its own edit when the server commits it,
  exactly like everyone else.
- Video and local players can share a room. The server renders frames only for
  video players.
- The status panel shows local tick, server steps/s, verified fingerprints,
  applied commands and resync count. Status reads `LOCAL SIM`, `PAUSED`,
  `SYNCHRONIZING`, `RESYNCHRONIZING` or `RECONNECTING`.
- Failures before the first synchronization are permanent and stop the
  client, because they would repeat on every retry. Examples: a server without
  local-sim support, or a world it cannot checkpoint (Newtonian gravity, Lua).
  Use the normal video client for those rooms.

### Rendering equivalence

The four-player test checks that a local client's final canvas is
**byte-identical** to the server's rendering of the same paused world, in both
the normal and heat views. A manual check also covered the pressure view.
Exception: fire, plasma and similar glow effects use a renderer-local
random flicker. They differ cosmetically between any two renderers (including
two server frames). The physics state underneath is still verified.

## 3. Divergence detection and recovery

On a fingerprint mismatch the client:

1. **discards the local world immediately.** It is never shown or edited again.
2. sends `{"type":"desync_report","tick","revision","fields":{group: digest}}`.
   The server keeps per-group digests for the last 32 hashed seals and replies
   `desync_detail` with the names of the differing groups. It logs
   `Replica player N diverged at tick T: differing groups=...`. Reports are
   diagnostic only, accepted only from local-sim peers, limited to 1/s and
   bounded in size.
3. reconnects, after the reply or a 2 s timeout, and receives a fresh exact
   checkpoint. Unconfirmed edits are counted as uncertain and are **not**
   replayed (unchanged policy).

Other replica errors (missed or reordered command, bad seal) take the same
discard-and-rejoin path. If a slow client overflows its 4096-message inbox,
that is treated as a recoverable disconnect.

Test hook: `--debug-corrupt-at-tick N` adds +1.0 to one pressure cell once at
local tick ≥ N. In the test, a single pressure perturbation at tick 30 had
spread to 7 of the 14 state groups by the next fingerprint (tick 60). The
client then resynchronized at tick 124 and verified every later fingerprint.

## 4. Measurements (loopback, this build host, 2 vCPU)

Settled water-and-dust scene in a metal tray, physics about 58 steps/s,
measured over the final 15 s of a 50 s run. Application payload only.

| Client | Median receive | Picture updates/s | Fingerprints verified |
|---|---:|---:|---:|
| `tpt-remote` (video, lossless) | 0.46 Mbps | 15 | n/a |
| `tpt-remote --local-sim` | 0.05 Mbps | 35 (local render, CPU-bound here) | 48, 0 resyncs |

The local client's remaining traffic is mostly one JSON `tick_seal` per
physics tick plus presence. Batching seals (task 5 in the 31 Oct doc) is the
next bandwidth step. Busier scenes widen the gap, because video cost grows
with on-screen change while command traffic does not.

## 5. Tests

`multiplayer-local-client` (`tests/multiplayer/local_client_test.py`, about 25 s):

- **Four players:** two local-sim clients (one editing via `--commands`, using
  both the brush and the extended `tool`/`box` path) plus two video clients
  (one admin editor). Edits run concurrently and the room is then paused. The
  local normal and heat canvases must equal the server's renderings. Each
  local client must have 0 resyncs, ≥ 2 verified fingerprints, all 5 commands
  applied and none pending, and receive < 0.5 Mbps after joining.
- **Divergence:** a corrupted local client and an honest one share a room. The
  corrupted client must detect, report and resync exactly once (two
  synchronizations, two connections) and keep verifying fingerprints
  afterwards. The server log must name `air-fields`. The honest client must
  never resync.

Full suite (`meson test -C build-mp`): 16 pass. `multiplayer-recovery`
skips as root; it was run separately as an unprivileged user and passes.
`multiplayer-impaired-network` needs `tc` (iproute2), which this host lacks.

## 6. Next steps, in priority order

1. **Determinism qualification across platforms.** Every pass above is one
   binary on one CPU type. Build a Windows client and an ARM client, then run
   10k–100k-tick fingerprint comparisons against the Linux server. Until that
   passes, keep the video client as the default.
2. ~~**Batch tick seals**~~ Done: see `SEAL_BATCHING_2026-11-02.md`
   (0.048 → 0.017 Mbps).
3. **Join cost:** cache checkpoints and compress them with zstd; large worlds
   can approach the 64 MiB limit.
4. **Long WAN soak** with the local-sim client under netem (needs a host with
   `tc`), including slow clients that fall behind the seal stream.
5. **Native Windows build** of `tpt-remote` (MSVC/MinGW) and packaging.
