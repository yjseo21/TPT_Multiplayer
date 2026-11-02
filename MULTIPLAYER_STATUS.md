# TPT Multiplayer: status

As of 2 November 2026. Player and host guide: `README.multiplayer.md`.
Engineering notes: `docs/multiplayer/`.

## Done

- **Dedicated server with local physics on every player's computer:**
  - exact snapshots when joining;
  - ordered edits and tick seals;
  - a sync check every second, with an automatic fresh copy on mismatch.
- **Cross-platform agreement:** copies agree bit for bit across MSVC, GCC,
  MinGW and Apple clang, on x86-64 and ARM.
- **In the real game:**
  - a room dialog;
  - switching between the room and your own canvas;
  - a player list and other players' cursors;
  - a preview of your own pending strokes.
- **Admin-only global controls:** pause, step, clear, every simulation setting,
  resets, invert air, and the console's world commands.
- **Newtonian gravity** in rooms, with a solver that gives identical results on
  every computer.
- **One stickman per player**, up to four.
- **Everyone's tools:** decoration, property, signs, sign buttons, sample, and
  pasting stamps.
- **Lag isolation:** joins cost the room a few milliseconds, and a computer that
  can't keep up shows TOO SLOW and jumps ahead.
- **Security:** limits on password guessing, constant-time password checks,
  per-player edit budgets, and size limits on every message.
- **Rooms without a certificate:** `host:port` joins a plain room, like a
  Minecraft server. The game notes that the connection isn't encrypted.
- **Hosting securely:** a sandboxed systemd service
  (`packaging/multiplayer/tpt-server.service`). It runs as a throwaway user
  with no access to your files, with a system-call filter and resource
  limits. `docs/multiplayer/HOSTING_SECURITY.md` explains it.
- **Testing:**
  - every element's snapshot completeness;
  - every element, wall and tool over the network with two players;
  - randomized rooms with two editors (32 seeds locally);
  - hostile clients, about 20,000–30,000 messages per run;
  - 4,000 corrupted pasted saves;
  - two real games: a bunker and a snow bomb, each pasted from its own canvas.
- **Memory safety:** the attack, fuzz and room tests ran clean under
  AddressSanitizer and UndefinedBehaviorSanitizer.
- **CI** on Windows, macOS (Apple Silicon and Intel) and Linux runs all of the
  above except the sanitizer runs. A tag `mp-v<version>` publishes release
  zips.

## Not done

- **Not available in rooms:** decoration fill, custom Game of Life types, cut,
  undo and redo, and opening a whole save into the room.
- **Very slow computers:** they get TOO SLOW and periodic jumps ahead; there's
  no server-rendered video fallback in the game yet (`tpt-remote` has one).
- **Untested conditions:** no measurements over real internet links yet, and no
  manual session between two separate computers.
- **Unsigned binaries:** the macOS and Windows builds aren't code-signed.
