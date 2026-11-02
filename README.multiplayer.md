# TPT Multiplayer

Play [The Powder Toy](https://powdertoy.co.uk) together: up to four players
share one world on a dedicated server, using the normal game with its normal
interface. You can switch between the shared room and your own canvas at any
time, and paste stamps from one into the other.

This is a fork of The Powder Toy 100.1 (upstream commit `c205becb`). It isn't an
official TPT release.

## Download

Each [release](https://github.com/yjseo21/TPT_Multiplayer/releases) has a zip
for each platform:

| Platform | File |
|---|---|
| Windows (64-bit) | `TPT-Multiplayer-…-windows-x86_64.zip` |
| macOS (Apple Silicon) | `TPT-Multiplayer-…-macos-arm64.zip` |
| macOS (Intel) | `TPT-Multiplayer-…-macos-x86_64.zip` |
| Linux (64-bit) | `TPT-Multiplayer-…-linux-x86_64.zip` |

Each zip contains:
- `powder`: the game;
- `tpt-server`: the dedicated server;
- `tpt-remote`: a small command-line client;
- this guide and the licence.

On macOS the binaries aren't signed. If macOS refuses to open them, run
`xattr -d com.apple.quarantine powder tpt-server tpt-remote` in the unzipped
folder.

Everyone in a room needs the same release: the game and the server check this
when you join.

## Joining a room

1. Start `powder`.
2. Press **Ctrl+Shift+J** to open the room dialog. Fill in:
   - the address (`host:port` means `wss://host:port`);
   - your name;
   - the room's password;
   - the admin key, if you're the room's administrator.
3. Press **Join**.

| Key | Action |
|---|---|
| Ctrl+Shift+J | Room dialog: join, leave, or join another room |
| Ctrl+Shift+M | Switch between the room and your own canvas. The room keeps running and stays connected. |
| Arrow keys | Steer *your* stickman (in a room) |

Your own canvas works exactly as single-player, and it keeps its state while you're
in the room. Stamps you copy in either one can be pasted into the other.

You can also join from the command line:
`powder join:wss://host:port name:Alice`, with the password in the
`TPT_ROOM_TOKEN` environment variable.

## What works in a room

Everything works as in single-player, for everyone, with these differences:

- **Admin only:** pause, frame step, clear, all simulation settings (gravity
  modes, Newtonian gravity, air, edges, ambient heat and temperature, the
  Options dialog), resets, invert air, and the console's `set`, `create`,
  `delete`, `bubble` and `reset`. In builds with Lua, type them with `!` in
  front, as in normal TPT. Other players see a notice.
- **Everyone:**
  - all elements, walls and tools, with all brush shapes;
  - decoration;
  - the property tool;
  - signs and sign buttons;
  - sample;
  - copying, and pasting stamps up to 160 KB.
- **Your stickman:** each player has one, up to four players.
  - Players 1 and 2 get STKM and STKM2; players 3 and 4 get a fighter that
    they steer.
  - Place it with the stickman tool and steer it with the arrow keys.
  - Your name is shown above it.
- **You see what others do:** other players' cursors and names, and a player
  list in the status line.
- **Your own strokes appear instantly:** a see-through preview shows them
  until the room confirms them.
- **Not in rooms (yet):**
  - decoration fill (it floods by the colours your own screen shows, which
    differ between players);
  - custom Game of Life types;
  - cut (copy works);
  - undo and redo;
  - opening a save into the room.
- **Lua scripts** run on your own canvas only.
- **Display settings** (temperature scale, render modes, HUD) are yours alone.

## Hosting a room

```sh
export TPT_ROOM_TOKEN='a password for the players'
export TPT_ADMIN_TOKEN='a different password for you'
./tpt-server --port 34403
```

On Windows PowerShell, use `$env:TPT_ROOM_TOKEN = '...'` instead of `export`.

- **Other computers need TLS (`wss://`).** Start the server with a certificate
  and key, for example from Let's Encrypt:
  `--cert fullchain.pem --key privkey.pem`. Open the port (TCP 34403 by
  default) in your firewall and router.
  - The game connects without TLS only to rooms on the same computer
    (`ws://127.0.0.1:…`).
- **Starting world:** `--load world.cps` starts from a save, and `--paused`
  starts paused. Newtonian gravity in the save is kept.
- **Autosaves:**
  - `--autosave-dir DIR` keeps rolling autosaves.
  - `--recover` restarts from the newest one.
  - `--save world.cps` saves on shutdown.
- **Room speed:** `--tick-rate 60`, the physics rate.
- **Bandwidth:** each player uses about 0.05 Mbit/s while playing, whatever
  the world contains. A join sends the world once (about 1–3 MB), at up to
  `--bandwidth-kbps 8192` per player.
- **Security:**
  - Passwords are sent only inside the encrypted connection.
  - After 5 wrong passwords in a minute, an address must wait a minute.
  - Each player's edits are rate-limited.
  - The admin can lock the room and remove players.
- `tpt-server --help` lists every option.

## How it works

The server holds the official copy of the room. Each player's game runs the
physics itself, from an exact copy received on joining. After that only edits
and short "tick finished" messages travel, which is why bandwidth doesn't depend
on the number of particles.

Every second, each player's game checks its copy against the server's. If they
differ, it downloads a fresh copy automatically. Copies agree bit for bit across
Windows (MSVC), macOS (Apple clang, Intel and Apple Silicon) and Linux (GCC).

Newtonian gravity in rooms uses its own solver, which gives identical results
on every computer. The game's usual gravity solver can't, because of thread
timing and machine-specific FFT algorithms.

A slow player never slows the others:
- The server never waits for players.
- Joining costs the room only a few milliseconds.
- A computer that can't keep up shows **TOO SLOW** and jumps to the room's
  present instead of falling further behind.

Engineering notes are in `docs/multiplayer/`:
- `ROOM_COMPLETE_2026-11-02.md`: features and the protocol;
- `LAG_ISOLATION_2026-11-02.md`;
- `NEWTONIAN_GRAVITY_2026-11-02.md`;
- `LATENCY_2026-11-02.md`;
- `IN_GAME_MULTIPLAYER_PLAN.md`: the overall plan and status.

## Building from source

You need Python 3, Meson, Ninja and a C++20 compiler. On Windows, that's
Visual Studio with the "Desktop development with C++" workload.

```sh
python -m pip install meson ninja
python tools/multiplayer/build.py --test     # builds into build-native/ and runs the tests
python tools/multiplayer/package.py --name my-platform   # zips the result into dist/
```

Pushing a tag named `mp-v<version>` makes CI build and test all four platforms,
then publish the zips as a GitHub release.

## Licence

The Powder Toy and this fork are free software under the GNU General Public
License, version 3 or later (see `LICENSE`).
