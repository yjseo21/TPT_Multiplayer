# Performance pass, 2 November 2026

Network traffic and per-frame costs, measured on the development machine
(WSL, release build). `tpt-benchmark` prints every figure below.

## Results

| What | Before | After |
|---|---|---|
| Idle room, one local-simulation player | about 55 kbps | 25 kbps |
| Same, with `--seal-batch-ms 50` | — | 15 kbps |
| Fingerprint (once a second, server and every player) | 10–25 ms | 1.3–3 ms |
| Newtonian gravity solve (per tick while mass changes) | 3.9 ms | 1.4–1.6 ms |

The fingerprint was the one real smoothness problem. At 10–25 ms it took a
whole 60 fps frame, once a second, on every computer in the room.

## Changes

- **Compact tick seals.** A seal now carries only the tick, plus any metadata
  (revision, reset generation, pause) that changed since the previous seal.
  - Pause stays explicit while paused, because it releases batched seals at
    once.
  - When a later seal replaces a held or queued one, it keeps the earlier
    one's metadata, so the replica still checks every change.
  - Replicas skip the check for an omitted field. Commits have already
    verified those values, and the fingerprint covers everything.
  - A typical seal shrinks from 79 bytes to 32.
- **Const JSON lookups in seal batching.** `value["paused"]` on a non-const
  value inserted a `null` member; queued seals also gained `"digest":null`.
  Both wasted bytes, and the stricter replica would have refused them.
- **Fingerprint hashing:**
  - one multiply per 64-bit word instead of per byte;
  - whole arrays and particles hashed as memory, guarded by `static_assert`s
    on endianness, array types and the particle layout.
  - Every step is still a bijection of the running value, so any single changed
    word always changes the result.
- **Gravity column pass.** The column transforms now run across whole rows, so
  memory is read in order instead of copying each column out and back.
  - Every element sees exactly the same operations, so the results are
    bit-identical.
  - The pinned cross-platform digest (2792519806613943772) is unchanged.
- **Paste pacing:**
  - The game now charges a paste what the server charges (`ToolCommandCostLimit`),
    not `XRES*YRES`. Before, rapid pastes passed the game's pacing and were then
    refused by the server.
  - A paste is queued whole or not at all, so its upload chunks never go out
    without it.

- **Server paste budget (security).** The server used to unpack a pasted save
  (up to 16 MB) before charging the player's editing budget. A broken save was
  never charged at all. Pastes are now charged first, so even broken ones count
  toward the limit of about two a second. `admin_test.py` checks this with a
  stream of broken pastes.

`BuildId` is now `tpt-c205becb-mp3-ingame5`, since older builds refuse
compact seals.

## Not changed

- **Seal batching stays off by default.** It saves another 10 kbps, but holds
  empty-interval seals for up to the batch time, which widens every player's
  margin behind the room. Hosts on slow uplinks can turn it on with
  `--seal-batch-ms`.
- **TPT's own physics** (1–17 ms a tick in the benchmark scenes) is upstream
  code, and runs as it does in single-player.

## Joining from far away (1.0.3)

A player 80-100 ms away saw "synchronizing" and "reconnecting" forever. A
proxy adding 50 ms each way and capping the link reproduced it with a busy
room:
1. The join snapshot (1-4 MB) took many seconds on the slow link, while the
   room kept running.
2. The player then had hundreds of ticks to catch up.
3. The "too slow" rule jumped ahead after 10 seconds behind, even while the
   backlog was shrinking.
4. Jumping ahead meant downloading the room again over the same link, so the
   cycle repeated.

Changes:
- **Catching up isn't falling behind.** The behind clock restarts whenever
  the backlog has shrunk by a tenth (and at least 30 ticks). Only a computer
  that really can't keep up jumps ahead.
- **Progress shown:** "DOWNLOADING THE ROOM 45% (0.8 of 1.8 MB)", then
  "CATCHING UP (5 s to go)".
- **Smaller snapshots:** each 4-byte word's bytes are grouped by position
  before zlib level 6 (it was level 1). Busy worlds are 10-25% smaller, empty
  ones up to 70%. Compression stays off the room's thread. Older autosaves
  (the plain encoding) still load.
- **New test, `multiplayer-distant-join`:** a 1.8 MB room joined over a
  100 ms, 150 KB/s link. It downloads in about 12 s, catches up without
  jumping ahead, and verifies 45 fingerprints.

`BuildId` is now `tpt-c205becb-mp3-ingame6`.

## Identical physics on every platform (1.0.4)

Windows and macOS players on a Linux server resynchronized every few seconds:
their copy of the world disagreed with the server's within a second. Rooms
between the same operating systems were fine, and the tests only ever ran
those.

Causes, found with a new probe (`tpt-determinism`) that runs a busy mixed
world and a scene for every element on each platform:
- **System maths functions.** `sin`, `cos`, `atan2`, `pow`, `hypot` and `exp`
  differ in their last bits between glibc, Windows' UCRT and Apple's
  library. 9 of 194 element scenes differed between Linux and Windows
  (photons, neutrons, fireworks, explosions and others), and so did the busy
  world.
- **Fast-math.** The game is built with `-ffast-math` and `/fp:fast`, which
  let each compiler reorder and approximate arithmetic its own way.
- **Denormals.** Windows and Linux builds flush them to zero, but macOS builds
  didn't.
- **Evaluation order (macOS).** C++ doesn't fix the order in which a call's
  arguments, or the operands of `+`, are evaluated. GCC and MSVC go right to
  left, clang left to right. In 16 places two random draws shared one call,
  as in `create_part(-1, x + rng.between(-1, 1), y + rng.between(-1, 1), ...)`,
  so a Mac spawned particles at a different neighbour. Found when CI's Intel
  Mac produced a different fingerprint, then reproduced with clang on Linux.

Fixes:
- **`simulation/DeterministicMath`:** `sin`, `cos`, `atan2`, `exp`, `log`,
  `pow` and `hypot`, built only from IEEE-exact operations, with a barrier
  against fused multiply-add. They're within 1-3 units in the last place of a
  double (`pow` within 63). All 163 calls in the simulation and brush code use
  them now.
- **Strict floating point in multiplayer builds:** `-fno-fast-math
  -ffp-contract=off` and `/fp:precise`. Physics speed is unchanged in the
  benchmark scenes.
- **Rooms flush denormals on every CPU,** including ARM64 (`FPCR.FZ`).
- **Fixed draw order:** `Simulation::RandomNeighbour` draws x, then y, and the
  sand colour noise uses one draw per statement. A script checks that no
  statement in the simulation makes two unsequenced random draws. Paste
  placement uses a stable sort, so particles stacked on one pixel keep their
  order on every C++ library.
- **Fireworks crashed any program without a renderer** (a room server that
  never served a video player): their colour table was empty. Every room world
  now fills the tables.

Verification:
- **The probe:** Linux GCC, Linux clang and Windows (MinGW, UCRT) produce
  identical fingerprints at all 216 checkpoints. They also match the value
  CI's Intel Mac computed.
- **A real room:** a Windows client against a Linux server, with 28 edits,
  verified all 40 fingerprints with no resyncs. Before the fix it diverged at
  tick 60.
- **CI:** `multiplayer-determinism` pins the fingerprint, so CI checks that
  MSVC, Apple clang on both CPUs, and GCC all compute it.

`BuildId` is now `tpt-c205becb-mp3-ingame7`.
