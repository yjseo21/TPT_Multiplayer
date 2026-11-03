# Hosting a room safely

What opening the server's port to the internet exposes, what was done about
it, how it was tested, and how to run the server so that even an unknown bug
can't reach the rest of your computer.

## The short version

1. Run the server as the sandboxed service in
   `packaging/multiplayer/tpt-server.service` (setup steps below), never as
   your own user or as root.
2. Open only TCP 34403 on your router and firewall.
3. Use a room password and an admin key you don't use anywhere else.
4. Keep the server updated.

With that setup, someone attacking the room can at worst disturb the game
itself: draw, paste, or try to crash or slow the room. They can't read, change
or run anything else on your computer. That's because of what the server
accepts, and because the sandbox would contain it even if the server had a bug.

## What an attacker can send, and what happens to it

The server listens on one port and speaks one protocol: WebSocket messages
of JSON, up to 4 KB each. Anyone can connect.

- **Clients never supply a path, a command or code.** The server only opens
  files chosen on its own command line: the starting save, autosaves and its
  preferences file. It contains no Lua or other scripting, and runs no other
  programs.
- **Every message is size-limited before it's parsed:** 4 KB for messages,
  frames refused before their payload is read, nested JSON depth-limited. Every
  field is type- and range-checked.
- **Passwords:**
  - Nothing works until a client knows the room password.
  - 5 wrong passwords or admin keys in a minute lock that address out for a
    minute.
  - Comparisons take the same time whatever the input, so timing reveals
    nothing.
  - Admin actions need the admin key; the server checks this itself.
- **Pasted saves are the most complex input:**
  - only this game's own save format is accepted;
  - at most 224 KB packed and 16 MB unpacked, checked from the header before
    anything is unpacked (the game itself allows 200 MB);
  - each save is fully parsed on the sender's own connection thread, so a
    broken one never reaches the room;
  - about three pastes or loads per second per player at most (loads are for
    administrators only).
- **Resources are bounded:**
  - 32 connections and 4 players;
  - each player's edits have a budget: 120 commands and 20 million brush
    cells a second, about 60 ms of server time a second per player at most;
    one command is at most about 3 ms, so no edit holds up a tick for long;
  - each connection's outgoing queue is capped in messages and in bytes
    (64 MB); a client that stops reading is dropped;
  - connections that don't log in within 5 seconds are closed.
- **Room pacing:** a logged-in player reports how fast their computer runs the
  physics, and the room slows to it. The server only stores that number, and
  the room never runs below half its configured speed, so a player who lies
  can at most halve the room's speed while they're in it.

## How it was tested

- **Hostile clients** (`multiplayer-hostile-input`):
  - Two logged-in attackers, one with the admin key, plus streams of
    anonymous connections, send random and malformed messages: broken JSON,
    nesting bombs, wrong types, huge and negative numbers, unknown operations,
    mutated valid commands, oversized text, bogus paste uploads, binary
    frames, garbage control messages.
  - That's about 20,000–30,000 messages per run, while an honest player
    verifies every fingerprint and an honest editor's edits must all apply.
    The server stays up and stops cleanly.
- **Corrupted saves** (`multiplayer-paste-fuzz`): a real save containing every
  element is unpacked, corrupted in seven different ways, packed again with a
  correct header, and sent through the same checks as a player's paste. Saves
  that get through are loaded into two worlds, which must come out identical.
  4,000 cases passed.
- **Memory safety:** the server, the client and the tests were built with
  AddressSanitizer and UndefinedBehaviorSanitizer. The hostile-client test,
  the paste fuzz, randomized rooms and the core tests ran clean, with no
  memory errors and no undefined behaviour. (Only UBSan's `vptr` check was off;
  it gives false reports when, as here, RTTI and no-RTTI code are mixed.)
- **The sandbox:** the server ran and served a player under the service's
  system-call filter, no-write-and-execute memory, and network-only socket
  restrictions.
- **Every push:** CI runs the hostile-client test, the paste fuzz,
  randomized rooms and the rest of the suite on Windows, macOS and Linux.

## Setting up the service on Linux

```sh
sudo install -m 755 tpt-server /usr/local/bin/tpt-server
sudo install -m 644 tpt-server.service /etc/systemd/system/tpt-server.service
sudo install -m 600 /dev/null /etc/tpt-server.env
sudoedit /etc/tpt-server.env
sudo systemctl daemon-reload
sudo systemctl enable --now tpt-server
sudo ufw allow 34403/tcp
```

In `/etc/tpt-server.env`, put these two lines:

```
TPT_ROOM_TOKEN=your-room-password
TPT_ADMIN_TOKEN=your-admin-key
```

Useful commands:
- `systemctl status tpt-server`: is it running?
- `journalctl -u tpt-server`: its log.
- `sudo systemctl restart tpt-server`: after an update.
- To start from a save: copy it into `/var/lib/tpt-server/` and add
  `--load /var/lib/tpt-server/world.cps` to `ExecStart` (via
  `sudo systemctl edit tpt-server`).

What the sandbox means:
- **Identity:** the server runs as a user systemd creates for it alone, with
  no password, no home and no files of its own.
- **Files:** the whole system is read-only to it, and `/home` and `/root` are
  invisible. It can write only to `/var/lib/tpt-server`, for autosaves.
- **Privileges:** it has none and can't gain any.
- **System calls:** only those an ordinary network service needs.
- **Sockets:** internet sockets only.
- **Resources:** capped at 2 GB of memory, 128 threads and 2 CPU cores.

## Running it as your own user instead (systemctl --user)

`tpt-server@.service` runs rooms under your own account, without root, one
room per folder in `~/services/powdertoy/`. For a room called `NAME`:

```sh
mkdir -p ~/services/powdertoy/NAME/data ~/.config/systemd/user
install -m 755 tpt-server ~/services/powdertoy/NAME/tpt-server
install -m 600 /dev/null ~/services/powdertoy/NAME/server.env
nano ~/services/powdertoy/NAME/server.env
install -m 644 tpt-server@.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now tpt-server@NAME
sudo loginctl enable-linger "$USER"
```

In `server.env`, put three lines; each room needs its own port:

```
TPT_ROOM_TOKEN=your-room-password
TPT_ADMIN_TOKEN=your-admin-key
TPT_PORT=34403
```

`loginctl enable-linger` keeps your services running while you're logged out
and starts them at boot. Useful commands:
- `systemctl --user status tpt-server@NAME`: is it running?
- `journalctl --user -u tpt-server@NAME`: its log.
- `systemctl --user restart tpt-server@NAME`: after an update.

This unit works on stock Ubuntu 24.04 and later. Those releases restrict
unprivileged user namespaces (`kernel.apparmor_restrict_unprivileged_userns`),
and a user service can't hide files or mount a read-only system without one.
So it keeps only the restrictions that system-call filters provide:
- **Privileges:** none, and it can't gain any.
- **Sockets:** internet sockets only.
- **System calls:** the system-service set, without privileged, resource,
  mount or debugging calls.
- **Memory and namespaces:** no writable-and-executable memory, and it can't
  create namespaces.
- **Resources:** capped memory, tasks and CPU.

It was tested with user namespaces disabled: the room started and served a
player.

**The trade-off:** it runs as you, with your file access, like a Minecraft
server started the same way. If a bug in it were ever exploited, the attacker
could read what you can read, such as `~/.ssh`. For the stronger isolation:
- **The system service** (`tpt-server.service`, above) runs as a throwaway
  user with a read-only system and no access to `/home`. It needs root to set
  up once, but doesn't change any system security setting.
- **Or keep the user service** and let systemd sandbox it. Allow user
  namespaces for systemd's service starter only, with an AppArmor rule:
  `profile systemd-executor-userns /usr/lib/systemd/systemd-executor
  flags=(unconfined) { userns, }`. The previous version of this unit then
  hides your home folder, keeping only the room's folder.

## What remains

No networked program can honestly be called impossible to break.

- **Denial of service:** someone can still try to slow or crash the room.
  Everything above limits that, and `Restart=on-failure` brings the room back
  within seconds.
- **Plain connections** (`ws://`):
  - A plain room's traffic, including the room password, can be read by anyone
    on the network path, such as a shared Wi-Fi.
  - That's why the password must be one you use nowhere else.
  - With a certificate, `wss://` encrypts everything.
  - Either way, the password only lets someone into the room. It's never your
    computer's password, and it grants nothing else.
- **Unknown bugs:** a memory bug in the server or its libraries (the
  WebSocket library, Mbed TLS, or TPT's save loader) that testing didn't find
  could in principle be exploited. The sandbox is there for exactly that case:
  even then, the attacker would be stuck inside it, as a user with no files,
  no privileges, a read-only system and network sockets only.
- **Players:** a hostile room can send its players bad data too. The game
  checks everything it receives with the same strictness, but players should
  still only join rooms they trust.
