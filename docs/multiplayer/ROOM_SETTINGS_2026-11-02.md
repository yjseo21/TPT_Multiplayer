# TPT multiplayer: simulation settings in rooms

**Engineering handoff, 2 November 2026.** The settings part of P5 in
`IN_GAME_MULTIPLAYER_PLAN.md`, done after P3b.

## What works

A room's simulation settings are the room's, and every player gets the same
values at the same tick. While a room is shown, these change the room's
settings for everyone:
- the mode shortcuts:
  - **W**: gravity mode (vertical, off, radial, custom);
  - **Y**: air mode;
  - **U**: ambient heat;
  - **Ctrl+E**: edge mode;
- the **Options** dialog: heat simulation, ambient heat, water equalisation,
  air mode, edge mode, gravity mode with custom X/Y, ambient air temperature,
  edge pressure, edge velocity X/Y, vorticity and convection;
- the quick-option buttons for ambient heat and the sand effect.

How it works:
- Only the room's admin can change settings. Other players get a notice.
- A change is a `settings` room command (admin only, validated), ordered with
  edits, recorded in the journal, and applied by `ApplySettings` on the server
  and on every player's machine.
- Floats travel as JSON numbers, which round-trip a float exactly, so every
  machine gets the same bits. All these settings were already in the exact
  checkpoint and the fingerprint, so a late joiner gets them and any
  divergence would be detected.
- Until the room applies a change, the dialog, the quick options and the
  shortcuts read back the requested value, so a control doesn't flick back for
  one round trip. Pressing W twice in quick succession still steps twice.
- On your own canvas everything works as in single-player, and your own
  world keeps its own settings.
- The protocol changed, so the build ID is now `tpt-c205becb-mp3-ingame2`.
  Older clients cannot join newer rooms.

## Fixed along the way

Before this change, three controls in a room changed the room's world on your
own computer only:
- the **Options** dialog changed it in place;
- the **sand effect** quick option toggled it the same way;
- the Newtonian gravity checkbox could start gravity there.

The room's fingerprints would catch the difference and force endless resyncs.
Now they all go through the room command, or are refused.

## Newtonian gravity

Newtonian gravity is not supported in rooms yet:
- TPT computes it on a worker thread whose timing differs between machines.
- Turning it on in a room is refused with a notice, both on your computer and
  on the server.
- The server now refuses to start from a save that has it on, and says how to
  fix the save. Before, it turned the gravity on and then no player could join.
- It needs a deterministic mode that runs in lockstep with the physics. That
  is the next item for gravity.

## Tests

- `multiplayer-core`:
  - parsing and admin-only access;
  - range, type and unknown-field rejection, including Newtonian gravity;
  - exact recording of float values;
  - the effect of `ApplySettings`, including solid edges becoming walls;
  - a Newtonian gravity save is refused, leaving the world untouched.
- `multiplayer-game-room` (real game):
  - W twice in one frame, Y, edge mode, ambient heat, and the Options dialog's
    custom gravity;
  - Newtonian gravity is refused;
  - the room ends with radial→custom gravity, pressure off, solid edges and
    ambient heat on, while the player's own world keeps vertical gravity;
  - the watcher verifies every fingerprint without resyncing, and a rejoined
    player gets the room's settings.

CI fixes in the same change:
- The rejoin step now waits until the rejoined game has synchronized, instead
  of a fixed 3 s. Snapshots go out at the 8 Mbit/s per-player cap, so a rejoin
  with several MB of air data takes 2–3 s on macOS CI.
- The "room keeps running under the dialog" check uses explicit marks instead
  of the once-per-second log rows.

## Still open

- Resetting air, sparks and ambient heat, and the sign, decoration and
  property tools (P5).
- Stamps and pasting saves (P4).
- Per-player stickmen.
- Deterministic Newtonian gravity.
