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
