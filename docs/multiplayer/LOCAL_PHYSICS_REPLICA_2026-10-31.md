# TPT multiplayer: local simulation / authoritative command replication

**Engineering handoff — 2026-10-31 — EXPERIMENTAL implementation, with compiled Linux runtime tests.**

**What is actually working:** an authoritative, headless Linux server, an independent
headless Linux replica process, a versioned exact checkpoint transferred over
WebSocket as 32-KiB chunks, ordered edits, tick-completion seals, exact local
physics execution, periodic state-fingerprint verification, and a locally
rendered PNG. Real TCP/WebSocket end-to-end tests now pass, including a
four-participant room (three simulation replicas and one editing client).
The local client does **not** receive video frames.

**What is not finished:** an interactive GUI for the replica, automatic
recovery on divergence (currently fail-closed), sustained/WAN and slow-client
qualification, guaranteed cross-CPU/compiler/OS determinism, mod and Lua
support, Newtonian gravity snapshots, and production-grade security review.
Do not mistake a 10-second localhost test for a released multiplayer product.
The older `tpt-remote` is still a working *video* client and can edit the world.

## 1. Architecture, in one diagram

```
  TPT server, exact physics owner (single world-owner thread)
      |  commands from an authenticated editor (WebSocket JSON)
      v
  validate request -> mutate World -> assign TickTimeline event ID
      |                                      |
      |                                      v
      |                     broadcast command_commit to replicas
      v                                      |
  advance authoritative physics ------------+
      |
      v
  broadcast tick_seal(tick, revision, generation, periodic state hash)
      |
      +---> replica A: apply commands -> advance locally -> verify hash -> render
      +---> replica B: apply commands -> advance locally -> verify hash -> render
      +---> replica C: apply commands -> advance locally -> verify hash -> render
      |
      +---> old video client (optional; receives compressed canvas frames)
```

At join, the server snapshots the full world **at a quiescent simulation
boundary** after processing that iteration's edits and physics. The server
sends a `checkpoint_begin` control JSON, a strictly ordered sequence of binary
chunks, then `checkpoint_end`, and finally an initial `tick_seal` (including
when paused). After this boundary, queued commits and seals are in
WebSocket FIFO order. The server's renderer skips all `localSimulation=true`
peers entirely. Local render is completely separate from physics verification.

The server remains authoritative: untrusted users may submit only the same
validated editing operations as the regular network client. The local replica
cannot send its own physics to overwrite the server.

## 2. Source files to read, in order

1. `src/multiplayer/World.{h,cpp}` — authoritative physics stepping and edits;
   `BeforeSim(false)` after an edit preserves allocation/map state.
2. `src/multiplayer/TickTimeline.{h,cpp}` and `CommandRecord.{h,cpp}` —
   monotonic event stamps and the canonical semantic command codec.
3. `src/multiplayer/ExactCheckpoint.{h,cpp}` — bit-preserving serialization;
   **only** when gravity threads/Lua callbacks are disabled, and **only**
   at quiescent world boundaries.
4. `src/multiplayer/CheckpointWire.{h,cpp}` — bounded chunk transport and
   strict reassembler, independent of simulation dependencies.
5. `src/multiplayer/Server.cpp` — optional `hello.localSimulation`, initial
   bootstrap and sender queue, `command_commit`, coalesced `tick_seal`,
   skip server canvas render for replica peers.
6. `src/multiplayer/ReplicaState.{h,cpp}` — state machine; welcome,
   checkpoint assembly and exact restore, ordered command replay, physics
   stepping only to sealed ticks, digest checking. Throws on divergence.
7. `src/multiplayer/Replica.cpp` — standalone WebSocket replica program,
   bounded receive mailbox, strict authentication/URL/TLS options, final PNG.
8. `src/multiplayer/StateDigest.{h,cpp}` — diagnostic *noncryptographic*
   checksums for particle slots, maps, air, gravity, RNG, settings, etc.
9. `src/multiplayer/meson.build` — targets and standard regression tests.
10. `tests/multiplayer/ReplicaTests.cpp`, `checkpoint_wire_standalone.cpp`,
    `replica_network_test.py`, `replica_four_player_test.py` — test harnesses.

**Earlier milestone specifications** remain in `docs/multiplayer/`; these are
historical snapshots and may state that runtime tests or local simulation were
not yet available. This document supersedes those status statements.

## 3. Wire protocol

Transport: authenticated WebSocket; only loopback `ws://` is accepted by the
replica CLI, while nonloopback connections MUST use `wss://` with verified
certificates. Keep `TPT_ROOM_TOKEN` private. `ProtocolVersion=3` and current
`BuildId` must match exactly; the protocol is experimental and not yet
negotiated for backward-compatible extensions.

A client initiates with:

```json
{"type":"hello","protocol":3,"build":"tpt-c205becb-mp3","name":"Replica","token":"your-token","localSimulation":true,"presence":false,"webp":false}
```

Actual `BuildId` must match the compiled project, not a copied example.
The server replies with its ordinary `welcome` plus `localSimulation:true`.

Server bootstrap controls (values illustrative; all counters are unsigned):

```json
{"type":"checkpoint_begin","id":1,"bytes":111000,"tick":0,"revision":0,"resetGeneration":0,"paused":false}
```

**Binary chunk:** 24-byte header + at most 32768 bytes; bytes `TPTC`
(ASCII magic), one-byte version=1, three reserved zero bytes, little-endian
`uint64` transfer ID, `uint32` total checkpoint bytes, `uint32` byte offset,
then the fragment payload. Receiver validates ID, version, reserved bits,
byte offset, full-size consistency, count, final partial chunk, and limits
one complete checkpoint to **64 MiB**. Chunks are never silently reordered.
The underlying checkpoint is itself versioned, zlib-compressed, CRC-checked,
and length-checked, but the CRC is not an authenticity guarantee.

```json
{"type":"checkpoint_end","id":1}
```

Then the server sends an initial `tick_seal`, even for a paused world:

```json
{"type":"tick_seal","tick":0,"revision":0,"resetGeneration":0,"paused":false,"digest":"16-character-hex-or-null"}
```

User edit broadcasts are canonical semantic operations, not per-pixel changes:

```json
{"type":"command_commit","id":1,"epoch":0,"tick":0,"ordinal":1,"afterTick":0,"revision":1,"resetGeneration":0,"actor":2,"command":{"op":"brush","...":"canonical command fields"}}
```

`command_commit` is applied **before** physics ticks after its declared
`tick`. `tick_seal` commits the interval: the replica may step from its current
tick to the seal's `tick`, but not beyond it. The replica checks tick,
revision, reset generation, pause status, event ordering, and (when supplied)
`DigestSummary(DiagnosticDigests(world))`. One hash accompanies the initial
seal and every 60th physics tick; a paused edit produces another hash. The
server coalesces adjacent seals **only** when no edit sits between them.

The digest is a deterministic 64-bit FNV-style checksum, useful for detecting
accidental divergence but not intended to authenticate a malicious server.

Other messages (presence, telemetry, room changes, ordinary ACKs) are
separate and do not advance local simulation.

## 4. How to build

Official dependency path (x86-64 Linux, GCC/Clang C++20, Meson, Ninja,
project's bundled dependency pack):

```sh
cd TPT/powdertoy
meson setup build-mp --buildtype=release \
  -Dbuild_multiplayer=true -Dstatic=prebuilt -Dlua=auto \
  -Dhttp=true -Dresolve_vcs_tag=no
meson compile -C build-mp
meson test -C build-mp --print-errorlogs \
  multiplayer-checkpoint multiplayer-checkpoint-wire \
  multiplayer-replica multiplayer-local-replica-network \
  multiplayer-local-replica-four-player
```

**This container lacked Meson**, and the package index was unreachable. To
avoid shipping untested code, the developer compiled the full simulation
engine and both networking executables with a *temporary, local CMake/Ninja
build* linking against the same shipped static dependencies. The source syntax
and full runtime tests PASSED in that Linux environment. The scratch CMake
file used container-absolute paths and is **not a supported build system**;
use the Meson path above on a prepared developer workstation. Meson target
integration itself was not executed here.

The included `tpt-server` and `tpt-replica` Linux executables in the separate
binary release are x86-64 test binaries, compiled unoptimized with the local
system's glibc/ELF dependencies; do not distribute them as production builds.

## 5. Running locally

Terminal A:

```sh
export TPT_ROOM_TOKEN='change-this-for-your-room'
export TPT_ADMIN_TOKEN='use-a-separate-admin-secret'
./build-mp/src/multiplayer/tpt-server --port 34403 --seed 12345 --tick-rate 30
```

Terminal B (the *noninteractive* exact-world client):

```sh
export TPT_ROOM_TOKEN='change-this-for-your-room'
./build-mp/src/multiplayer/tpt-replica \
  --url ws://127.0.0.1:34403 --name Physics1 \
  --run-for 15 --min-seals 100 --output ./replica-final.png
```

To edit concurrently, run the existing `tpt-remote` SDL client (or any
protocol-3 editor) against the same server with the matching room token.
The replica generates its **own** rendered `replica-final.png`; it does not
request any video frames. `--min-seals N` can make the client exit earlier;
`--run-for` is a safety deadline. A paused room produces an initial seal but
will not reach a high `--min-seals` until edits or steps occur.

The headless replica prints `tick`, `seals`, `edits`, `matching_hashes`,
`wire_payload_bytes`, `checkpoint_payload_bytes`, and
`control_payload_bytes`; counts are WebSocket *application payload*, not
ethernet/IP/TCP/TLS overhead. For remote use, configure `--url wss://...`
and `--ca your-ca.pem`, and configure a certified TLS listener on the server.

## 6. What was verified on Linux (2026-10-31)

| Gate | Result |
|---|---|
| GCC C++20 standalone checkpoint chunk test, including malformed packets | **PASS** |
| Compiled `ExactCheckpointV1` engine test (full restore, corrupt rejection, allocation and 120 ticks) | **PASS** |
| Compiled local replica engine test (checkpoint + edit + 120 ticks + deliberate hash failure) | **PASS** |
| Extended independent physics replay (1000 **and 10,000** ticks with water/fire) | **PASS** |
| Live server + replica in paused session, PNG render, checksum | **PASS** |
| Live server + replica with 3 edits over 75 running ticks, hash at tick 60 | **PASS** |
| Live 3 independent local replicas + 1 editing client, 6 edits and 81 ticks, hashes and PNGs | **PASS** |
| Full Meson compilation/test suite | **NOT RUN** (Meson unavailable here) |
| Cross-OS, cross-compiler or cross-CPU reproducibility | **NOT RUN** |
| Long-duration WAN/slow-client test, automatic desync recovery | **NOT RUN / NOT IMPLEMENTED** |

Tests can be rerun using the binaries built above:

```sh
python3 tests/multiplayer/replica_network_test.py \
  build-mp/src/multiplayer/tpt-server build-mp/src/multiplayer/tpt-replica
python3 tests/multiplayer/replica_four_player_test.py \
  build-mp/src/multiplayer/tpt-server build-mp/src/multiplayer/tpt-replica
```

The longer engine test accepts a tick count as its single argument:

```sh
./build-mp/src/multiplayer/tpt-replica-tests 1000
./build-mp/src/multiplayer/tpt-replica-tests 10000
```

`ReplicaTests.cpp` contains synthetic water and fire. Other element families
and full checkpoint state matrices still need coverage. `--run-for` and source
build/ABI mismatch are real runtime constraints.

## 7. Actual measured bandwidth

Measurements: GCC Linux build, loopback TCP/WebSocket, no TLS, small synthetic
playground, 30 physics ticks/s target, default 8-Mbit/s room upload budget,
including all JSON and binary WebSocket message *payloads* seen by the C++
replica. TCP/IP/WebSocket framing, authentication handshake and editor traffic
are not included. They are NOT WAN measurements and NOT worst-case scenes.

| Scenario | Checkpoint payload | Subsequent control payload | Total per replica |
|---|---:|---:|---:|
| Paused, 3 steps + 6 ordered edit commands | 111,415 B | 2,268 B | 113,683 B |
| Active, 75 tick seals + 3 edits | 111,416 B | 6,954 B | 118,370 B |
| Four participants: 3 replicas + 1 editor; per replica | 111,417 B | 8,167 B | 119,584 B |
| Four participants: sum for 3 replicas | 334,251 B | 24,501 B | 358,752 B |

The four-participant run reached tick 81 and verified two state fingerprints
per replica. After bootstrap, approximately 8.2 KB per replica over roughly
2.7 s of tick advancement corresponds to ~24 kbit/s of application payload,
**not a sustained network throughput guarantee**. Six edit commands were
processed. Importantly, the command-only traffic does not carry a periodic
rendered framebuffer. Server `CanvasProducer` doesn't capture images for
replica-only rooms.

A large/saturated world could produce an initial snapshot approaching the
64-MiB transfer cap. In that case initial join may be slow or rejected;
this remains a separate bandwidth/UX problem from routine physics updates.

## 8. Known correctness boundaries / hazards

- **Determinism:** same source, same compiler flags, same CPU family and
  bitwise-compatible floating-point math are not guaranteed to generalize
  across OS/architecture. Matching seeds and event order do not fix FMA,
  excess precision, libm, thread scheduling, or hidden global data.
- **Checkpoint restrictions:** active gravity worker and Lua callbacks are
  expressly rejected. The server currently fails replica bootstrap clearly
  instead of substituting lossy state. The standard video client still works.
- **Desync:** any mismatch throws, stops the replica process and requires
  manual restart/rejoin. Implement automatic rejoin/recovery without silently
  accepting incorrect physics. The server should continue authoritative
  operation despite one replica's failure.
- **Performance:** local physics is not free. A slow client can queue behind
  the authoritative server. Its 256-message receive mailbox, server-side
  2048-control queue and 10,000-tick gap guard limit memory growth and work,
  but do not yet constitute a complete latency/queue management strategy.
- **Build identity:** `BuildId` identifies a source version, not exact
  compilation flags or hardware behavior. Two builds that say they match can
  still diverge. Add a strong engine feature/ABI fingerprint before general use.
- **Hash coverage:** diagnostic categories are broad but may omit future
  or static element state; digests are not cryptographic checksums.
- **Render:** headless replica saves one PNG at exit. There is no interactive
  local SDL display and no per-user rendering options in this executable.
- **Network security:** `ws://` is loopback-only for the replica; `wss://`
  requires verified CA, server certificate, admission token and permission
  handling. A malicious peer could still trigger resource-intensive
  checkpoint creation or excessive state transfers; review defenses.
- **Protocol:** no transfer cancellation/generation negotiation, compression
  dictionary, chunk retry, or partial checkpoint patch. WebSocket preserves
  ordering on one connection, but a dropped connection needs a fresh join.
- **Backward compatibility:** `localSimulation` is opt-in. Old video clients
  remain on the legacy stream; never send binary TPTC chunks to them.

## 9. Next engineering tasks, in suggested priority

1. **Turn `tpt-replica` into a responsive SDL game client.** Reuse the existing
   `tpt-remote` UI / `EditSession`, but drive its framebuffer from the local
   renderer on each committed seal instead of `DecodeCanvas()`. Preserve
   cursors, drawing tools, admin permissions and the video fallback.
2. **Automatic divergence recovery.** On digest mismatch, record first
   divergent subsystem(s), stop physics advancement, reconnect to obtain
   a fresh authoritative checkpoint, then resume at a seal boundary.
   Respect authentication and transfer limits; never replay unacknowledged
   edits silently after reconnection.
3. **Determinism stress suite:** 10k–100k ticks on independently launched
   processes; cross-compiler/OS/CPU; dense high-entropy water/fire/electronics,
   portals, GoL, signs, extra particle allocations, and scripts. Include
   desync-first-field reporting and a reproducible seed+command journal.
4. **Headless four-player WAN stress:** 30–60 minutes with artificially
   restricted throughput, added RTT/jitter/loss, slow replicas, queue sizes,
   dropped clients and resume, big checkpoints; measure actual p50/p99
   command-to-visible latency, server CPU, client CPU, bytes and failures.
5. **Reduce even the ~24 kbps control rate:** encode a batch of many tick
   seals as a compact binary range (startTick/endTick, revision, flags),
   with edits interposed at exact tick boundaries. A server may need to send
   *only one sealed tick range per small delay*, rather than 30–60 JSON
   messages/s. Add a monotonic ack to keep slow clients bounded.
6. **Warm join / snapshot optimization:** frame-neutral zstd compression,
   chunk deduplication, content-addressed checkpoint segments, use saved
   periodically cached checkpoints and a short event-log catch-up to avoid
   per-join full serialization CPU. Cap join memory and rate limit aggressively.
7. **CI/release:** verify Meson targets with MSVC, macOS and GCC/Clang; use
   sanitizers/fuzzers for wire, zlib and exact checkpoint readers; audit
   SDL and static library ABIs; publish reproducible build manifests.

The best long-term architecture remains **deterministic local physics +
authoritative event ordering, with video as a fallback**. Sending particle
deltas is not necessary if independent replicas can stay bit-identical.
