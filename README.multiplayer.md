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
   - the address: `host:port` (for example `myroom.duckdns.org:34403` or
     `203.0.113.5:34403`), or `wss://host:port` for a room with a certificate;
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
`powder join:host:port name:Alice`, with the password in the
`TPT_ROOM_TOKEN` environment variable.

## What works in a room

Everything works as in single-player, for everyone, with these differences:

- **Admin only:** pause, frame step, clear, all simulation settings (gravity
  modes, Newtonian gravity, air, edges, ambient heat and temperature, the
  Options dialog), resets, invert air, and the console's `set`, `create`,
  `delete`, `bubble` and `reset`. In builds with Lua, type them with `!` in
  front, as in normal TPT. Other players see a notice.
- **Loading a save into the room (admin only):** in the room, open a save
  from the save browser (Open) or a local file. It replaces the room's world
  and settings for everyone, as opening a save does in single-player, and the
  room stays paused if it was. Saves up to 224 KB. To open one on your own
  canvas instead, press Ctrl+Shift+M first. Other players who open a save
  switch to their own canvas, as before.
- **Everyone:**
  - all elements, walls and tools, with all brush shapes;
  - decoration;
  - the property tool;
  - signs and sign buttons;
  - sample;
  - copying, and pasting stamps up to 224 KB.
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
  - undo and redo.
- **Lua scripts** run on your own canvas only.
- **Display settings** (temperature scale, render modes, HUD) are yours alone.

## Hosting a room

Like a Minecraft server: run `tpt-server`, forward TCP port **34403** on your
router, and give players your address (a domain name such as a free DuckDNS
name, or your public IP).

**On a Linux PC that stays on, use the sandboxed service.** It comes as
`tpt-server.service` in the Linux zip. It runs the server as a throwaway
user with no access to your files, and restarts it if it stops. Setup takes
six commands; see `HOSTING_SECURITY.md`, which also explains what opening the
port exposes and how that was tested. To run rooms under your own account
(`systemctl --user`, from `~/services/powdertoy/NAME`), use
`tpt-server@.service`; the same document covers it.

With a room password set, the server accepts players from other computers.
Without one, only this computer can join.

To try it quickly by hand:

```sh
export TPT_ROOM_TOKEN='a password for the players'
export TPT_ADMIN_TOKEN='a different password for you'
./tpt-server --port 34403 --autosave-dir saves
```

On Windows PowerShell, use `$env:TPT_ROOM_TOKEN = '...'` instead of `export`,
and allow TCP 34403 in Windows Defender Firewall.

- **Passwords:** use ones you don't use anywhere else.
  - A plain room's connection isn't encrypted, so the room password can be
    read by anyone on the network path (a shared Wi-Fi, for example). It only
    lets someone into the room; it's never a password for your computer.
  - For encryption, start the server with a certificate and key, for example
    from Let's Encrypt (`--cert fullchain.pem --key privkey.pem`), and have
    players join `wss://host:port`.
- **Starting world:** `--load world.cps` starts from a save, and `--paused`
  starts paused. Newtonian gravity in the save is kept.
- **Autosaves:**
  - `--autosave-dir DIR` keeps rolling autosaves.
  - `--recover` restarts from the newest one.
  - `--save world.cps` saves on shutdown.
- **Room speed:** `--tick-rate 60`, the physics rate.
- **Bandwidth:** each player uses about 0.025 Mbit/s while playing, whatever
  the world contains (`--seal-batch-ms 50` brings it to about 0.015). A join sends the world once (about 1–3 MB), at up to
  `--bandwidth-kbps 8192` per player.
- **Security:**
  - 5 wrong passwords in a minute lock that address out for a minute.
  - Every message is size- and range-checked.
  - Pasted saves are limited and checked before they reach the room.
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

Slow computers and heavy rooms:
- Joining costs the room only a few milliseconds.
- Each player's game tells the server how fast it can run the physics. When a
  big explosion or a full screen is too much for one player's computer, the
  room slows down to that player's pace (**ROOM SLOWED**), down to half its
  normal speed, and speeds back up once the load passes. Nobody falls behind
  or has to download the room again.
- A computer too slow even for half speed doesn't hold the others back: it
  shows **TOO SLOW** and jumps to the room's present, waiting longer before
  each further jump.

Engineering notes are in `docs/multiplayer/`:
- `HOSTING_SECURITY.md`: hosting safely;
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
