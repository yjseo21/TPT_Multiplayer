# Plan: multiplayer inside the real game

**2 November 2026.** Target: the normal Powder Toy (`powder.exe`), with its own
interface, can join a room on a dedicated server, play on the shared canvas,
switch back to the player's own single-player canvas at any time, and paste
saves and stamps into the room. Gameplay should feel like the original:
the full interface, 60 fps rendering, and no lag beyond what the network
physically requires.

## 1. What the measurements allow

`tpt-rollback-benchmark` (Linux, WSL, this machine's CPU; release build):

| Scene | Physics step | Snapshot | Restore | Exact checkpoint save / load | Snapshot replays exactly? | Checkpoint replays exactly? |
|---|---:|---:|---:|---:|:-:|:-:|
| water, 30k particles | 5.8 ms | 3.5 ms | 7.4 ms | 110 / 37 ms | **no** | yes |
| fire on wood, 35k | 6.1 ms | 2.6 ms | 7.1 ms | 99 / 39 ms | **no** | yes |
| mixed, 60k | 11.7 ms | 3.2 ms | 9.0 ms | 106 / 42 ms | **no** | yes |
| dust, 180k | 19.9 ms | 7.2 ms | 13.3 ms | 142 / 42 ms | **no** | yes |

Consequences:

1. **Rollback prediction is ruled out.** Correcting for a remote edit 100 ms
   late means re-running about 6 physics steps: a 40–130 ms frame hitch every
   time anyone else edits. The engine costs too much per step for rollback.
2. **Upstream undo snapshots are not exact.** Restoring one and re-running
   diverges from an uninterrupted run. They cannot carry multiplayer state;
   only `ExactCheckpoint` can (exact, but about 100 ms, so join/resync only).
3. The client already pays the same per-step physics cost as single-player
   mode. A client that runs exactly one step per tick, never re-simulating, has
   single-player frame cost. That is the smoothness guarantee to keep.

## 2. Latency design

**Correction (P2, 2 November).** The first version of this plan proposed
scheduling each edit `D` ticks ahead and claimed your own edit would appear
after one-way latency plus `D`. That is wrong. Without speculation, a client
can only show its own edit after the server has ordered it and said so: one
round trip. Letting clients choose the tick does not help either, since the
server must not have sealed that tick yet and its seals take one-way latency
to arrive. Every arrangement gives the same floor. Scheduled ticks would add
protocol complexity for no latency gain, so they were dropped.

What can be removed is every millisecond **above** one round trip. P2 did
that:

- The server applies an edit as soon as it arrives, at the current tick, instead
  of on the next tick (up to 16.7 ms, about 8 on average, saved).
- One seal per tick by default (`--seal-batch-ms 0`); batching is optional.
  Unbatched costs about 0.05 Mbps.
- The client's margin behind the newest seal follows the measured gaps between
  seal arrivals instead of a fixed batch length (2 ticks on loopback).
- While your own edit is in flight, the client closes that margin at up to one
  extra tick per frame, so the edit lands as soon as its tick is sealed.

Measured on loopback (RTT about 0, so this is pure overhead): own edit to
local world **7.6 ms median, 13 ms p95**, from 60 / 90 ms unbatched and
153 / 258 ms at the old 100 ms batch. That is under one 60 fps frame. Expected
totals are therefore about **RTT + 8 ms**: a frame on a LAN, about 60 ms at
50 ms ping. Other players' edits appear after their own one-way trip, the
server's broadcast and the 2-tick margin.

The remaining RTT is hidden visually in P3: your stroke is drawn at once as a
ghost preview (render-only, not physics) until the real material arrives.

## 3. Phases and acceptance gates

### P1. Foundation refactor (no behavior change)
Status 2 November: **done**.

- Client: `Session.{h,cpp}` holds the socket, reconnect, control handling, the
  ordered local-simulation stream, pacing, divergence recovery, video decode
  and acknowledgements, probes and status. `Client.cpp` went from 877 to 492
  lines.
- Server: `Peer.{h,cpp}` holds the per-connection sender, moved verbatim.
  `Room.{h,cpp}` owns all room state as members, replacing the locals `main`
  shared with its callbacks and the two function-`static` seal trackers. The
  200-line connection callback is now `Room::Accept` and `OnMessage`, which
  dispatches to `Hello`, `Cursor`, `Probe`, `DesyncReport` and
  `QueueRequest`. The tick loop is `Room::Step()`: `CollectPeers`,
  `BroadcastPresence`, `ApplyRequests`, `Advance`, `BootstrapReplicas`,
  `SealTick`, `Measure`, `PublishFrames`, `Autosave`, `Stats`. A journal
  failure now stops the room through `Room::Failed()` instead of a global
  flag. `Server.cpp` went from 956 to 204 lines (options, world loading,
  socket server, shutdown).
- Code was moved by script (dedent plus renames: `Seconds()` to the identical
  `MonotonicSeconds()`, `options.` to `config.`), so logic is unchanged.
- Gate passed: full suite 17/0/1 (unchanged); Windows build and native unit
  tests pass; the cross-platform harness passes against the refactored
  server. The Windows-hosted `tpt-server.exe` ran a room with a Windows editor
  and replica (30 fingerprints verified). The reverse direction, Linux
  replicas on a Windows server, is untested: WSL cannot reach Windows
  loopback, and other addresses require TLS plus a firewall rule.

- Pull the connection logic out of `Client.cpp`'s 800-line `main` into a
  reusable `multiplayer::Session`: connect, hello/welcome, reconnect with
  backoff, ordered inbox, `ReplicaState`, pacing, desync report and resync.
  `tpt-remote` becomes a thin shell over it; the game will use the same class.
- Server: split `Peer` into its own files; move the main loop's state
  (including function-`static` seal trackers) into a `Room` class.
- Gate: the full suite and the cross-platform harness pass unchanged.

### P2. Latency floor (done)
Status 2 November: **done**. Scheduled ticks were dropped (section 2). Shipped:
early application on the server (`Room::WaitForRequests` / `ApplyEarly`), an
unbatched seal default, an adaptive pacing margin, own-edit catch-up, and
own-edit latency measurement (`editLatencyMs` in `tpt-remote --metrics`;
`tools/multiplayer/seal_batch_benchmark.py`). Details:
`LATENCY_2026-11-02.md`.

Follow-ups noted, not done: binary seal framing (only bandwidth, about 0.05
Mbps today); latency under netem, which needs root for `tc`; video clients
still wait for the next frame (`--update-rate`, 50 ms at the default 20/s).

### P3. The real game joins a room
Status 2 November: **P3a done** (`IN_GAME_P3A_2026-11-02.md`): join, show and switch, drawing tools routed, other paths refused, real-game test. **P3b done** (`IN_GAME_P3B_2026-11-02.md`): in-game room dialog (Ctrl+Shift+J), player list, other players' cursors, render-only preview of pending own edits, and the room keeps running under dialogs. **Room settings done** (`ROOM_SETTINGS_2026-11-02.md`): gravity, air and edge modes and every Options-dialog simulation setting are admin room commands; Newtonian gravity is refused. **Lag isolation done** (`LAG_ISOLATION_2026-11-02.md`): joins no longer stall the room (snapshot compression off its thread, 3.6-5.8 ms copy), and a too-slow computer shows TOO SLOW and jumps ahead instead of falling behind. **P4 and P5 done** (`ROOM_COMPLETE_2026-11-02.md`): admin-only resets and console commands, four player stickmen, decoration, property and sign tools, sign buttons, and pasting stamps. **Newtonian gravity done** (`NEWTONIAN_GRAVITY_2026-11-02.md`): a solver that gives identical results everywhere. Release packaging, a player and host guide (`README.multiplayer.md`) and password-guessing limits added. Still open: loading a whole save into a room, custom Game of Life types, a video fallback for very slow computers.

- `GameModel` gets a simulation host: **Local** (today's single-player,
  untouched) or **Session**. Switching canvases swaps the host. The
  single-player world stays in memory, unchanged, while you are in a room.
- One editing interface for every world-changing path. Single-player calls the
  simulation directly, exactly as now. A session turns each call into a
  command. Paths to route (from a survey of `GameController`):
  `DrawPoints/DrawLine/DrawRect/DrawFill/ToolClick/ToolDrag` (all tools,
  including property, sign, decoration and walls), `PlaceSave`,
  `LoadStamp/LoadClipboard`, `CutRegion`, `ClearSim/ReloadSim`,
  `SetPaused/FrameStep`, `ResetAir/ResetSpark/SwitchGravity/SwitchAir/
  ToggleAHeat/ResetAHeat`, edge mode, ambient heat and air settings, and
  stickman keys (`player.comm`, written directly today).
- Undo is unavailable in rooms (ambiguous with other players). Lua and the
  console cannot change a room's world. A render-only preview for pending own
  edits. Menu entries: Join, Back to my canvas, players, room admin.
  `powder --join ws[s]://host:port`.
- Gate: every routed path has a session test; no session path writes the
  simulation outside `World::Apply` (enforced by test); the interface is the
  upstream interface.

### P4. Saves and stamps in rooms
- `PlaceSave` command carrying the serialized save (size-capped, re-parsed and
  validated by the server, deterministic placement including transforms). This
  covers stamps, clipboard paste and copying from your own canvas into the room.
- Admin "load save into room": new reset generation and a fresh checkpoint for
  everyone. Cache and zstd-compress checkpoints to cut join time.

### P5. Coverage of the game's features
- Stickmen as per-player input commands; signs; decorations; portals, wifi,
  life and the rest in the determinism harness.
- Newtonian gravity: deterministic synchronous mode plus checkpoint support.

### P6. Qualification
- MSVC and ARM determinism (the harness exists), WAN soak, and a 4-player
  frame-time comparison with single-player on the same worlds.

## 4. Cleanup worth doing along the way

- `Client.cpp` / `Server.cpp`: monolithic `main` functions with long lambdas
  (fixed in P1).
- Per-tick JSON for seals (P2 binary framing).
- `ExactCheckpoint` saves take about 100 ms, mostly zlib. Encoding once per
  join and caching it matters more than raw speed. Try zstd in P4.
- Upstream physics optimization is out of scope: any change to it must keep
  bit-identical results across platforms, so it is high-risk and needs the
  determinism harness on every change.
