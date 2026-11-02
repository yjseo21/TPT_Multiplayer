# TPT multiplayer: admin controls, four stickmen, every tool, pasting

**Engineering handoff, 2 November 2026.** This closes phases P4 and P5 of
`IN_GAME_MULTIPLAYER_PLAN.md`, except for Newtonian gravity.
Build ID: `tpt-c205becb-mp3-ingame3`.

## Who may change what

| Action | Who | How |
|---|---|---|
| Pause, frame step, clear | admin | room command, as before |
| Simulation settings: gravity mode and custom gravity, air mode, edge mode, ambient heat and temperature, heat simulation, water equalisation, sand effect, edge pressure and velocity, vorticity, convection | admin | `settings` (see `ROOM_SETTINGS_2026-11-02.md`) |
| Reset air, sparks or ambient heat, invert air (shortcuts and menus) | admin | `reset` |
| Console `set`, `create`, `delete`, `bubble`, `reset` (Lua builds: `!set` and so on) | admin | `console` |
| Drawing: elements, walls, simulation tools, decoration, property tool | everyone | `tool` |
| Signs: add, edit, move, delete; pressing button signs | everyone | `sign`, `sign_button` |
| Pasting stamps and copied areas | everyone | `paste` |
| Your own stickman | each player | `stickman`, `stickman_spawn` |
| Temperature scale, decoration display, render modes, HUD | per player | local display only, as in single-player |

A player who isn't the admin gets a notice in place of the change. The server
also refuses admin commands from non-admins.

Some things remain local in rooms:
- **Lua scripts** act on your own canvas. The game says so the first time Lua
  runs while a room is shown.
- **Room speed** is set by the server's `--tick-rate`; the client FPS cap
  doesn't change it.
- **Undo, redo, reload and saving** apply to your own canvas only.
- **Cut** is refused in rooms; copy works.

## Four stickmen, one per player

- The server gives each player a stickman slot, 1–4, the lowest free one when
  they join. It's reported in `welcome` and in the presence roster.
- Slot 1 is STKM and slot 2 is STKM2, so saves and the elements behave exactly
  as in single-player.
- Slots 3 and 4 are fighters (FIGH) controlled by their player: a fighter with
  `playerst::controller` set skips its AI. That field is part of the snapshot
  and the fingerprint, and it's reset whenever a fighter slot is reused.
- **Placing:** in a room, the STKM and STKM2 tools place *your* stickman.
  Placing it again moves it, killing the old one, so there's never a
  duplicate. AI fighters can still be drawn with FIGH.
- **Steering:** the arrow keys steer your stickman, with the same key logic as
  single-player (`comm`, `pcomm`). Key changes skip the stroke queue, and so do
  placements, so they stay in order with each other.
- **Leaving:** when a player leaves while holding a key, the server stops their
  stickman with a command of its own (actor 0, its own sequence numbers).
- **Names:** each player's name is drawn over their stickman.

## Tools

- **Decoration** (SET, CLR, ADD, SUB, MUL, DIV, SMDG): each stroke carries its
  colour and is replayed through `ApplyDecorationPoint`, `ApplyDecorationLine`
  and `ApplyDecorationBox`. **Decoration fill stays refused:** it floods by the
  colours each player's own renderer shows, which differ with display settings.
- **Property tool:** the stroke carries the property and value chosen in the
  property window. It's replayed by a copy of `PropertyTool`'s brush, line and
  rectangle code, and fill uses `flood_prop`.
- **Signs:** in a room, the sign window edits a private copy and never changes
  the shown world. Signs are part of the fingerprint, so a local edit would
  cause a resync. On OK, Delete or leaving the window, it sends the change.
  - An edit applies only if the sign is still where its editor saw it, which
    guards against concurrent edits.
  - Button signs spark the conductor under them, for everyone.
- **Sample tool:** reads the shown world, as in single-player.
- **Custom Game of Life types:** still refused, because their rules are local
  to each player.

## Console commands

- The sender resolves names, temperature units and selectors into exact values
  (`property`, `value`, `selector` all / index / point / type). Every machine
  then applies the same thing.
- `bubble` places its 31 SOAP particles from a fixed offset table, computed
  once from the console's own `cosf`/`sinf` rounding. Math library results
  differ slightly between platforms, and every machine must place the same
  particles.

## Pasting

- The placed save (after rotation and flipping) is serialized as a save file
  and uploaded as base64 chunks (`paste_chunk`, 3,000 characters each). The
  chunks use the player's operation budget and go through the paced outbox,
  with backpressure.
- The `paste` command then commits with the save inside it, so every player
  loads the same bytes with `Simulation::Load`, at a position in cells.
- Limits:
  - 160 KB of save file per paste;
  - ordered server messages up to 320 KB;
  - client-to-server messages are still 4 KB, so the upload goes in chunks.
- Holding Shift while placing leaves out pressure, as in single-player.

## Fixes found on the way

- **Server pacing:** the server reset its schedule after a late wake-up instead
  of catching up. On macOS CI runners, coarse sleeps made rooms run at 21–28
  ticks per second while each tick took about 1 ms. Late ticks are now made up,
  up to 8 ticks behind.
- **"Behind" for jumping ahead:** it was a backlog divided by a measured rate.
  A frame step while paused makes that rate about 1 per second, so a
  small backlog read as "84 s behind" and the client jumped ahead wrongly.
  Now "behind" means how long the client has been unable to catch up: more
  than half a second of ticks beyond its margin, 3 s for `TOO SLOW`, 10 s
  before jumping ahead.

## Tests

- `multiplayer-core` covers:
  - stickman slots, moving a stickman by placing it again, and a controlled
    fighter walking by its keys;
  - resets;
  - console set, create, delete and bubble;
  - decoration and property tools, with decoration fill refused;
  - signs, including a stale edit being ignored, a button sparking, and
    removal;
  - pasting, with exact recording and size and format checks.
- `multiplayer-game-room` (the real game, on the Lua build and the plain
  build) adds:
  - placing a stickman and walking it with the arrow keys;
  - pasting a copied floor;
  - an admin air reset;
  - the console's bubble, create and set;
  - decoration, property tool, two signs and a button press.

  The watcher still verifies every fingerprint without a resync, and agrees on
  every command.

## Still open

- Newtonian gravity: needs a deterministic, synchronous mode.
- Admin "load a whole save into the room": opening a save still switches to
  your own canvas.
- Custom Game of Life types in rooms.
- WAN latency measured under netem.
