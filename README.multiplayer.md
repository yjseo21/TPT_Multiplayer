## NEW: Playable local-physics client (2026-11-02)

Add `--local-sim` to `tpt-remote` to play with **physics simulated on your own computer**. The server sends one exact checkpoint, then only ordered edits and tick seals, with no video. The usual tools, cursors, views and admin keys all work. Fingerprint mismatches are detected, diagnosed by the server and repaired automatically by rejoining from a fresh checkpoint. Measured on one scene: about 0.05 Mbps per player against 0.46 Mbps for the video stream. Cross-platform determinism is **not yet qualified**, so the video client remains the default.

```sh
./build-mp/src/multiplayer/tpt-remote --url ws://127.0.0.1:34403 --name Alice --local-sim
```

Your own edits reach your local world about 8 ms after the network round trip (loopback median 7.6 ms). The server sends one seal per tick by default; `tpt-server --seal-batch-ms N` trades latency for bandwidth (about 0.05 Mbps unbatched). See [`docs/multiplayer/LATENCY_2026-11-02.md`](docs/multiplayer/LATENCY_2026-11-02.md).

Details, test results and next steps: [`docs/multiplayer/LOCAL_CLIENT_2026-11-02.md`](docs/multiplayer/LOCAL_CLIENT_2026-11-02.md). The noninteractive `tpt-replica` and its protocol are described in [`docs/multiplayer/LOCAL_PHYSICS_REPLICA_2026-10-31.md`](docs/multiplayer/LOCAL_PHYSICS_REPLICA_2026-10-31.md).

---

**Dedicated-server multiplayer prototype**

This fork adds `tpt-server` and `tpt-remote`. The server runs the upstream physics engine and renderer. Up to four clients send brush operations. The default shared canvas is lossless; an optional, explicitly negotiated WebP stream trades visual fidelity for bandwidth. Clients never advance authoritative physics locally. Normal, heat and pressure views remain independently selectable by each player.

Upstream base: `c205becb1cf7ef1915293e2c99b2c7901d2887cd` (The Powder Toy 100.1.401).
Protocol/build identity: `3` / `tpt-c205becb-mp3-seedfix1` (see `BuildId` in `src/multiplayer/Protocol.h`). Client and server builds must match; rebuild both when upgrading from an earlier prototype.

**31 October 2026 codec continuation (source only; not yet integrated-build qualified)**

The low-bandwidth WebP change adds `--webp-quality` and `--webp-scale` to the server. For a small shared upload link, start with `--webp-quality 30 --webp-scale 2 --upload-kbps 2000`; scale 2 averages each 2×2 source-pixel block and nearest-neighbour expands the decoded image. This **loses thin one-pixel features**, so choose `--webp-scale 1` for a sharper image. Defaults remain exact lossless. The server only uses WebP with clients that advertise `webp:true`; older clients remain lossless. When libwebp is not installed at build time, WebP mode cannot be selected. The existing protocol-v3 lossless modes remain supported, and the new lossy WebP mode is independently decodable.

See `tools/multiplayer/README-CODEC.md` for build dependencies, precise codec/handshake details, benchmark CSV interpretation, reproduction instructions, known limitations and the required integrated tests. The distributed pre-existing `linux-bin/` binaries do **not** contain this new functionality. Rebuild both executables before expecting WebP. The **project handoff** is `../HANDOFF_2026-10-31.md`.

**Build**

Use an existing C++20 compiler, Python 3, Meson and Ninja. The upstream dependency pack is downloaded and checksum-verified by Meson. IXWebSocket is fetched at the commit pinned in `subprojects/ixwebsocket.wrap`; the checked-in Meson overlay enables Mbed TLS and applies a bounded-message patch.

```text
meson setup build-mp --buildtype=release -Dstatic=prebuilt -Dlua=none -Dhttp=false -Dbuild_multiplayer=true -Dresolve_vcs_tag=no
meson compile -C build-mp
meson test -C build-mp --print-errorlogs
```

The normal `powder` desktop target remains enabled. Set `-Dbuild_powder=false` when only building multiplayer. Use a Visual C++ developer terminal for Windows. The current development build is compiled and tested under Ubuntu/WSL; native Windows compilation needs a C++ toolchain and separate verification.

Multiplayer executables are under `build-mp/src/multiplayer/`. On Windows their names end in `.exe`.

For this workspace's existing Ubuntu/WSL environment, PowerShell helpers are provided:

```powershell
.\tools\multiplayer\build-wsl.ps1 -Test
# Run these in separate terminals; the client appears through WSLg.
.\tools\multiplayer\run-wsl.ps1 server --paused
.\tools\multiplayer\run-wsl.ps1 client --name Alice
```

The launcher forwards `TPT_ROOM_TOKEN` and `TPT_ADMIN_TOKEN` from PowerShell into Ubuntu. Set them as described below before launching; an unset admin token means no client has administrator controls.

**Local play**

Start the server in a terminal:

```bash
export TPT_ROOM_TOKEN='choose-a-room-password'
export TPT_ADMIN_TOKEN='choose-a-separate-admin-password'
./build-mp/src/multiplayer/tpt-server --paused
```

In another terminal, set `TPT_ROOM_TOKEN` to the same value and launch:

```bash
./build-mp/src/multiplayer/tpt-remote --url ws://127.0.0.1:34403 --name Alice
```

Set `TPT_ADMIN_TOKEN` in a client's environment to grant that client global controls. Keep it unset for regular players. In PowerShell, use `$env:TPT_ROOM_TOKEN = '...'` and `$env:TPT_ADMIN_TOKEN = '...'` instead of `export`. Credentials are sent in the WebSocket handshake message and never included in application logs or URL query strings.

Open additional clients with different names. The fifth authenticated player waits and retries while the room is full. Physics pauses automatically when no authenticated players are connected. The server persists when clients leave. Clients automatically reconnect after transient connection failures or server restarts.

Reconnect attempts wait 1, 2, 4, 8, then 10 seconds between failures, resetting after a valid canvas is received. The last canvas stays visible with a `RECONNECTING` status, and drawing is disabled until a fresh canvas in the selected view arrives. The selected material, brush radius, name and view stay in memory. A new connection has fresh frame baselines, command sequences and clock measurements. A previously sent command with no received acknowledgement may or may not have executed; it is counted as unconfirmed and is never replayed. Authentication/protocol rejection and certificate-verification failures stop the client. Use `--no-reconnect` for fail-fast scripts, or `--run-for SECONDS` to bound a diagnostic run. `--view 0|1|2` selects the initial normal/heat/pressure view.

| Control | Action |
| --- | --- |
| Left mouse drag | Draw the selected material |
| Right mouse drag | Erase |
| 1–9 | DUST, WATR, STNE, FIRE, METL, SPRK, OIL, WOOD, erase |
| Mouse wheel | Brush radius, 0–16 |
| F1 / F2 / F3 | Normal / temperature / pressure display |
| F4 | Show/hide the connection and performance panel |
| Space | Pause/resume (admin) |
| F | Advance one step while paused (admin) |
| Ctrl+Backspace | Clear the world (admin) |
| Ctrl+L | Lock/unlock new ordinary-player joins (admin) |
| Ctrl+Shift+1–4 | Remove a non-admin player occupying the corresponding roster slot (admin) |
| Escape | Disconnect and close |

The title bar shows the selected material, radius, server tick, pause and room-lock state. The connection panel appears below the canvas, leaving the play area unobstructed. The four roster positions are numbered left-to-right, top-to-bottom; show the roster with F4 before kicking. Full upstream GUI integration is future work.

**Room administration**

Authenticated administrators (`TPT_ADMIN_TOKEN`) can lock new ordinary-player connections or remove a connected ordinary player. A lock does not disconnect existing players; a correctly authenticated admin can still join a locked room to restore access. The lock is in-memory and resets on server restart. Kicking disconnects that session with a policy close, preventing the native client from automatically reconnecting; **kicking is not a persistent identity ban**. Names are display labels rather than identities, and admins cannot kick themselves or other admins.

Administrative actions consume the same monotonically increasing per-connection `seq` as edits and receive normal `ack` or `error` responses, without changing the physics tick or world revision. Example messages:

```json
{"seq":1,"op":"admin","action":"lock","locked":true}
{"seq":2,"op":"admin","action":"kick","player":3}
{"seq":3,"op":"admin","action":"lock","locked":false}
```

The server includes `roomLocked` in `welcome`, broadcasting `{"type":"room","locked":true}` or `{"type":"room","locked":false}` after lock changes. Locked non-admin joins receive retryable WebSocket close code 1013. The native client supports administration commands from a JSONL file via `--headless --run-for SECONDS --no-reconnect --commands FILE.jsonl`; omit `seq` in the file, as the client assigns it. Supply the appropriate environment credentials. Older protocol-v3 clients may ignore the additive room state messages.

**Internet connections**

The server supports native TLS. Bind a public interface only with a certificate/key and a room credential:

```bash
./build-mp/src/multiplayer/tpt-server --bind 0.0.0.0 --port 34403 --cert fullchain.pem --key privkey.pem
./build-mp/src/multiplayer/tpt-remote --url wss://your-server.example:34403 --name Alice
```

The server machine needs an inbound TCP firewall rule, and port forwarding if hosted behind a router. Players make outbound connections. Clients verify certificates and hostnames. For a private CA, use `--ca ca.pem`. Linux clients default to `/etc/ssl/certs/ca-certificates.crt`; Windows clients use the current user's trusted root store. Unencrypted connections are limited to loopback by both executables.

This remains an experimental prototype. Synthetic bandwidth, slow-client, and four-player kernel network impairment tests pass locally. Sustained server capacity and real internet play still need qualification.

**Bandwidth**

The server streams original-resolution RGB pixels using compressed full frames, XOR deltas, or tiny unchanged-frame messages. It keeps physics-only particle fields on the server. Compression is lossless; there is no image-quality slider or reduced resolution. The full particle-state codec remains available for internal diagnostics and physics tests but is no longer sent over the network. Future particle inspection needs a server query.

`--upload-kbps 8192` sets the **whole room's** canvas budget in decimal kilobits/second (default 8.192 Mbps). `--bandwidth-kbps 8192` remains the per-player ceiling. Each connected player receives an equal share of the room budget, capped by their ceiling; unused shares are not borrowed. Shares adjust when players join or leave. The room permits one maximum-sized canvas packet of burst (about 709 KB total), charges a conservative 512-byte overhead allowance per packet, and preserves its budget across joins, resyncs and view changes. Control messages are separately bounded and outside this canvas budget; TCP retransmissions and actual wire overhead are not measured by it.

For a constrained upload, leave headroom for transport overhead and other traffic. For example, a 1.6 Mbps room cap with a 1 Mbps per-player ceiling:

```powershell
.\tools\multiplayer\run-wsl.ps1 server --upload-kbps 1600 --bandwidth-kbps 1000
```

When a client cannot keep up, the server sends newer captures less often and computes their deltas against that client's actual sent baseline. Physics continues. Initially only one packet can await acknowledgement; after the first ACK, at most two can. ACK delay (including transfer and decoding) increases the send interval immediately, with gradual recovery toward the configured update rate when ACKs become faster. An overdue ACK prevents adding more backlog. Waiting keyframes reserve shared credit so repeated small packets cannot starve them; inactive reservations expire. Ten seconds without acknowledging an outstanding frame disconnects the client. Paused, unchanged worlds send a 52-byte application heartbeat at most once per second, subject to pacing and budget. The native client also exchanges a small telemetry probe once per second. Extremely low budgets or upload speeds can still trigger synchronization/ACK timeouts on large scenes; no minimum update rate is guaranteed.

See [MULTIPLAYER_STATUS.md](MULTIPLAYER_STATUS.md) for reproducible before/after measurements. These are scene-dependent estimates, not a guarantee that every scene can run at 20 updates/sec under the default cap.

**Players and shared cursors**

Launch each client with a name, for example `--name Alice`. The status panel lists connected players with their server-assigned IDs and admin badges. Other players' brush outlines and names appear over the canvas; `*` beside a name means that player is holding a drawing button. These are cosmetic indicators, not confirmation that an edit was accepted. F4 hides the status panel; remote cursors remain visible.

Cursor updates are sent at most 10 times per second, with a one-second keepalive for stationary cursors. Leaving the window or losing focus hides the cursor. The server hides cursors after two seconds without an update and sends membership/cursor snapshots at most 10 times per second. Reconnecting clears the old roster and creates a new player ID. Names are limited to 32 UTF-8 bytes and control characters are replaced for display. Names need not be unique; use the ID to distinguish players.

Presence uses a separately bounded control stream outside the canvas upload budget. Senders keep only one pending presence snapshot; excessive cursor updates are dropped. Clients opt in with `"presence":true` in `hello`, and the server echoes support in `welcome`. Legacy clients still connect and appear in the roster without receiving presence messages. A `cursor` message contains `visible`, `drawing`, `x`, `y` and `radius`; identity is assigned by the server. Presence never changes the simulation or editing sequence numbers. Raw `--output` screenshots remain canvas-only; `--output-hud` includes remote cursors and the roster. JSONL metrics include the latest `players` array.

**World files and diagnostics**

`--load world.cps` imports a standard Powder Toy save at startup. `--save world.cps` atomically exports on graceful shutdown (Ctrl+C or SIGTERM). Use a new output filename when preserving earlier versions matters.

Enable periodic backups and recovery with:

```powershell
.\tools\multiplayer\run-wsl.ps1 server --autosave-dir multiplayer-data/autosaves --autosave-interval 60 --autosave-keep 5 --recover
```

Use the same command after a crash or restart. Autosaves are opt-in via `--autosave-dir`; its default interval is 60 seconds and retention is five generations. `--recover` loads the newest valid committed save, falling back to an older one when necessary. An empty directory starts a new world. If committed saves exist but none are valid, startup fails and preserves them. `--load` and `--recover` cannot be combined. Add `--paused` to force a recovered world to start paused; otherwise the save's pause setting is restored.

The server copies a standard save on the simulation thread. A worker compresses and writes it, flushes a temporary file, atomically commits a numbered `.cps`, then prunes old generations. POSIX also syncs the directory. The queue is bounded, unchanged paused worlds skip periodic writes, and graceful shutdown commits a final save. Recovery ignores incomplete `.tmp` files. An exclusive directory lock prevents two server processes from using the same backup directory. Save failures are logged and retried at the next interval without stopping the room; a final-save failure produces a nonzero server exit. Existing committed files remain available if a write fails.

These are ordinary TPT saves, not exact transient-state checkpoints. A crash can lose changes since the last committed backup, and some runtime-only engine state is not represented by the standard save format. Tick/revision counters and connection state restart. Keep independent copies if you need history beyond the configured retention. Periodic autosave and restart recovery were tested both on Linux temporary storage and this workspace's Windows-mounted drive under WSL; native Windows builds remain unverified.

`--tick-rate 60` and `--update-rate 20` configure desired simulation/capture rates. Overload slows wall-clock simulation; the server preserves the engine's step semantics. Rendering and compression use worker threads, with one replaceable pending snapshot. Copying an immutable render snapshot still takes place on the simulation thread. `--run-for SECONDS` provides a bounded run for testing. The server logs ticks, operation revision, connected players, full/delta/repeat counts, and cumulative wire bytes for currently connected players every five seconds.

`tpt-benchmark` compares full-state and canvas traffic on four seeded synthetic scenes over 120 physics steps and 40 captures each. It measures physics, state encode/decode, canvas rendering and delta encoding. Bandwidth columns estimate 20 updates/sec before WebSocket/TLS overhead; they are not achieved network throughput measurements. Rendering timings include RGB packing and full-frame compression; delta timings cover per-client comparison/compression.

For an automated renderer check:

```bash
./build-mp/src/multiplayer/tpt-remote --headless --frames 10 --output remote.png
```

The status panel shows:

- **Ping:** application probe round-trip time, excluding time waiting in the server's reply queue. TCP retransmission and stream congestion still affect it; this is not ICMP ping.
- **Updates/s:** decoded canvas packets per second, including unchanged-pixel heartbeats; not the monitor refresh rate.
- **RX Mbps:** received application payload, including control messages, excluding WebSocket/TLS/IP overhead.
- **Simulation steps/s and step ms:** achieved physics rate over approximately one second and mean automatic physics-step cost. Rendering/snapshot-copy time is outside the step-cost number.
- **Canvas age:** estimated elapsed time from the server's immutable snapshot capture to now. A four-timestamp exchange estimates clock offset; asymmetric delay makes this approximate. The panel displays `paused` for a known-current paused canvas rather than warning about an intentionally frozen image.
- **Share / pace / in flight:** current room allocation, ACK-based delivery-rate ceiling, and outstanding frames versus the current window. JSONL metrics also include the total room cap, per-player ceiling and smoothed frame-ACK delay.

Status distinguishes connecting, reconnecting, synchronization, resyncing, paused, stale telemetry, slow simulation, active bandwidth throttling, delayed updates and high latency. The title bar also shows the status when the panel is hidden. Thresholds are diagnostic hints, not guarantees about the underlying cause. Measurements expire after five seconds without telemetry.

Capture diagnostic logs and a screenshot with the panel:

```powershell
.\tools\multiplayer\run-wsl.ps1 client --run-for 30 --metrics build-mp/client-metrics.jsonl --output-hud build-mp/client-status.png
```

Paths passed through the WSL launcher are Linux paths relative to the repository. `--output` still saves only the shared canvas; `--output-hud` includes the panel. `--headless --run-for SECONDS` performs the same measurements without opening a window. JSONL logs contain measurements, canvas identifiers/checksums, connection count, connection readiness, pending edits and cumulative unconfirmed edits, never credentials. A timed run that ends without a complete current canvas exits with an error.

**Protocol and concurrency**

WebSocket text messages carry small JSON control messages. Client hello:

```json
{"type":"hello","protocol":3,"build":"tpt-c205becb-mp3","name":"Alice","token":"room-password","adminToken":""}
```

After `welcome` and a complete frame, a client can submit:

```json
{"seq":1,"op":"brush","x1":100,"y1":40,"x2":110,"y2":40,"radius":4,"element":1}
```

Sequence numbers increase per connection. Accepted operations receive an `ack` with their sequence, physics tick, and global operation revision. Rejected requests return `error`; sequences consumed by rejected operations must not be reused. Duplicate sequences never edit the world twice. A disconnect creates a new session; unacknowledged commands are not automatically retried.

Network callbacks validate and enqueue bounded operations. The main server thread alone mutates the world. An independent worker renders immutable snapshots for the requested display modes, sharing each result across viewers. Each connection has its own sender, byte budget, last sent baseline, at most one pending capture and 32 control replies. Replacing an unsent capture never discards a delta dependency. Published pixels are immutable.

Binary messages start with `TPC3`. Their 52-byte header contains: magic (4 bytes); kind, view, pause flag and reserved byte (one byte each); frame ID, baseline ID, physics tick and operation revision (four little-endian uint64s); reconstructed-pixel CRC32 (little-endian uint32); and the server's monotonic snapshot-capture timestamp in microseconds (little-endian uint64). Kind 0 carries a zlib full frame with baseline ID zero. Kind 1 carries zlib-compressed XOR bytes against the specified baseline. Kind 2 carries no body and reuses baseline pixels. Pixels are three B/G/R byte planes, exactly `612*384*3` bytes after reconstruction. Full versus delta encoding chooses the smaller compressed body. CRC32 detects reconstruction errors; TLS provides connection integrity.

Clients acknowledge decoded frames with `{"type":"frame_ack","frame":123}`. Acknowledgements are cumulative and separate from editing sequences. Frame IDs increase per connection. Missing baselines trigger `{"type":"resync"}` and a new keyframe; `{"type":"view","view":1}` changes display mode (0 normal, 1 heat, 2 pressure) and also starts with a keyframe. Resync/view requests share a four-request burst and two-per-second sustained limit. New connections always start with a full frame.

`{"type":"probe","id":123}` returns `telemetry` with the same ID, server receive/send timestamps, physics measurements, and per-client stream status. The server accepts at most four probes per second. The client has one outstanding probe, normally sent once per second; unmatched and duplicate replies cannot replace the current clock estimate. Probe IDs and frame acknowledgements do not consume editing sequences.

Decoded canvas size is fixed, and packets are limited to `612*384*3+4096` bytes; malformed headers, trailing compression data, stale IDs and bad checksums are rejected. The client preserves delta order in a bounded queue. Application JSON messages are capped at 4096 bytes, and IXWebSocket additionally bounds frame/fragment accumulation at 32 MiB before reading large payloads. Per-message WebSocket deflate is disabled.

**Tests and remaining milestones**

The core tests compare the server's simulation stepping with upstream calls, exercise edits while paused, validate exact state and canvas round trips, compare all three views pixel-for-pixel with upstream rendering, verify missing-baseline detection and byte budgeting, and reject malformed packets/commands. Network tests start a real server and independent clients, verify four-way canvas agreement, player limits, authentication, permissions, duplicate handling, late join, independent views, keyframe resync, idle traffic, native rendering, save/load, and oversized transport frames. They also exercise a stalled reader, the adaptive one/two-frame acknowledgement window, resumed delta delivery, a 128 kbps budget, and clearing accumulated graphics. When OpenSSL is available, they generate a temporary certificate and verify trusted/untrusted TLS behavior.

`multiplayer-recovery` starts the native client before the server, drops its live TCP connection, forcibly kills the server twice, and verifies that the same client reconnects each time with a fresh baseline and its heat view restored. It checks periodic saves, retention, exclusive locking, write failures/retries, corrupt-newest fallback, ignored incomplete writes, graceful final save, all-corrupt startup rejection, permanent credential rejection and fail-fast mode. Edit-session core tests verify that unconfirmed operations are discarded rather than replayed. The storage-permission test needs an unprivileged Linux user and otherwise reports skip code 77. To retain its logs, saves and final screenshot:

```bash
python3 tests/multiplayer/recovery_test.py build-mp/src/multiplayer/tpt-server build-mp/src/multiplayer/tpt-remote build-mp/recovery
```

`multiplayer-local-client` runs two `--local-sim` clients and two video clients editing together. It requires the local normal and heat canvases to be byte-identical to the server's, then injects a local divergence and requires detection, server diagnosis and automatic resynchronization.

`multiplayer-administration` checks unauthorized commands, consumed/rejected sequences, room locks and admin bypass, kick protections, session removal, unchanged world revision, and native scripted moderation.

`multiplayer-traffic` checks a shared 512 kbps room budget with four live clients, equal shares, reallocation after departure and final canvas agreement. Its measured payload must fit one shared initial burst plus the configured sustained rate.

`multiplayer-impaired-network` starts a disposable unprivileged Linux user/network namespace and applies `tc netem` only to its loopback. It exercises four clients at nominal 50/100/200 ms RTT, jitter, 1/1/3% actual packet loss, shared 8/4/2 Mbps limits, and a temporary reduction to 512 kbps. Two clients use the native executable (including SDL's dummy display), two independently decode/acknowledge the stream and issue edits. Tests verify telemetry, actual packet drops, physics progress, display-mode switching, adaptive window bounds and eventual agreement on the paused canvas. The test exits with skip code 77 if Linux, iproute2 or unprivileged namespaces are unavailable; a skip does not qualify the build for impaired networks.

The default `build-wsl.ps1 -Test` includes this test. To keep measurements and screenshots:

```bash
python3 tests/multiplayer/network_impairment_test.py build-mp/src/multiplayer/tpt-server build-mp/src/multiplayer/tpt-remote build-mp/impairment
```

Next: sustained real-internet sessions, deployment qualification, and broader UI integration before expanding the full tool UI. Exact transient-state checkpoints remain future work. This version exposes basic material brushes only, with global pause/step/clear restricted to administrators. A bandwidth cap bounds usage but does not guarantee a minimum display rate for every scene or connection.

The engine retains its upstream GPL license. IXWebSocket is MIT-licensed; its license is included in the fetched dependency. Mbed TLS and other existing dependency notices remain in the upstream dependency distribution.


## Experimental deterministic-session journal (source milestone, 31 Oct 2026)

This source now includes server-assigned tick/ordinal command commits, optional `commandCommits:true` subscription for research clients, and fixed-seed replay diagnostics. Build the **new** server and `tpt-replay` before using these flags. On a blank room:

```sh
./build-mp/src/multiplayer/tpt-server --paused --seed 12345 --journal session.jsonl
./build-mp/src/multiplayer/tpt-replay --journal session.jsonl
```

This is not a working client-side physics transport; the client still receives canvas frames. See `docs/multiplayer/LOCKSTEP_SPIKE_2026-10-31.md` for limitations, exact test commands, state coverage and engineering handoff. Do not treat CPS or TPM1 render frames as exact deterministic checkpoints.
