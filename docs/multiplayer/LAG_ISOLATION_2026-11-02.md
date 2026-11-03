# TPT multiplayer: one lagging player must not make others lag

**Engineering handoff, 2 November 2026.**

## Already isolated (unchanged)

- **The room never waits for players.** There is no lockstep: the room's
  thread ticks at its own rate whatever any player is doing.
- **Each player has their own send thread and queue.**
  - While a player falls behind, consecutive empty tick seals merge into one
    message. Seals that carry a fingerprint are always kept.
  - Past 2,048 queued messages, the server drops that player alone. Their game
    rejoins from a fresh snapshot.
- **Each player checks their own copy against the fingerprints in the seals.**
  A lagging player never holds up anyone else's checks.
- **Edits have a per-player budget** of 120 commands per second and 20 million
  brush cells per second (3 million before 4 November).

## Fixed: a join, rejoin or resync stalled the room for everyone

Before, the snapshot a joining player needs was copied and zlib-compressed on
the room's thread. That was about 60–85 ms per join for a busy world. In the
100,000-particle test, the room averaged 52.6 ticks per second over the
5 seconds around one join, against 59.7 normally. Every player saw a hitch.

Changes:
- **Compression moved to the joining player's sender thread.** It happens once
  per snapshot (`CheckpointSource`). Players who join at the same moment share
  one snapshot. The `checkpoint_begin` byte count is filled in after
  compression.
- **Faster raw copy, same format.** On little-endian machines (every supported
  platform), arrays of 4-byte values and particles (fourteen 4-byte fields, in
  format order, checked at compile time) are copied in one block. The format
  is unchanged.
- **Reused snapshot buffers** (`SnapshotBuffers`, two kept, one warmed at
  startup). Fresh memory for a 25 MB capture cost more than the copy itself.

`tpt-benchmark` now reports snapshot costs. The raw copy is 25 MB.

| Scene | Copy (room thread) | Compression (sender thread) |
|---|---|---|
| static metal | 4.6 ms | 40 ms |
| moving water | 3.0 ms | 52 ms |
| dense dust | 2.8 ms | 68 ms |
| fire | 3.5 ms | 48 ms |

Measured in a room with about 100,000 particles and one late joiner. The
server's stats line now reports `longest_step_ms`, `snapshots` and
`snapshot_capture_ms`.

| Version | Copy per join | Longest room step around a join | Average rate |
|---|---|---|---|
| Before | 60–85 ms (copy and compression) | not logged before this change | 52.6 ticks/s |
| Compression moved off the room thread | 37–48 ms | about 60 ms | 59.8 ticks/s |
| Plus faster copy and reused buffers | 3.6–5.8 ms | 25.7 ms (normal peak 20.5 ms) | 59.8 ticks/s |

## Fixed: a computer too slow for the room fell further and further behind

Before:
- When far behind, the client stepped its whole backlog in one frame. On a
  computer that can't keep up, each catch-up froze it for longer.
- Eventually the replica queue overflowed (8,192 messages), the client
  resynced, and that triggered the join stall above.

Now (`Session::Pump`):
- **Catch-up has a time budget of 12 ms of physics per frame**, with at least
  one tick when one is due. The game stays responsive and falls behind visibly
  instead of freezing.
- **"TOO SLOW" status.** If the client has been unable to catch up for 3 s
  (more than half a second of ticks beyond its margin), the status becomes
  `TOO SLOW`. This replaced a backlog divided by a measured rate, which a
  paused frame step could inflate; see `ROOM_COMPLETE_2026-11-02.md`. The game's status line shows
  "TOO SLOW FOR THIS ROOM (N s behind)". `behindSeconds` and `skipAheads` are
  in the metrics.
- **Jump ahead.** After 10 s unable to catch up, the client discards its local world and
  rejoins from a fresh snapshot of the room's present, at most once every
  30 s. Thanks to the first fix this costs the room a copy of a few
  milliseconds. It is not a divergence, so `resyncs` stays 0. Unconfirmed edits
  are not replayed, as with any reconnect.

## Tests

`multiplayer-slow-player` runs a server and two `tpt-remote` local-simulation
players. One player is made too slow with `--debug-step-delay-ms 100`, a
test-only flag that makes it run about 10 ticks per second against the room's
60. The test checks:
- the slow player reports `TOO SLOW`, jumps ahead at least once, catches up
  afterwards (under 3 s behind), stays under 15 s behind, and never diverges;
- the normal player stays in sync (`LOCAL SIM`, no resync, one connection,
  under 1 s behind);
- the server's average rate stays above 50 ticks/s in every stats window;
- each snapshot copy takes under 50 ms (typically 4 ms).

CI fixes in the same change:
- `game_room_test`: leaving the room is checked with an explicit mark. The
  room is left for only 0.9 s, which once-per-second log rows can miss.
- `local_client_test`: the honest player's fingerprint count scales with the
  ticks it actually ran. An overloaded macOS runner ran that room at 20 ticks
  per second.

## Still open

- Video mode as an option for computers too slow for local physics: the
  server streams pictures, which `tpt-remote` already supports and the game
  does not yet.
- A shared world's physics cost is shared by design. A huge world can make
  the room itself slow (`SERVER SLOW`) for everyone.
