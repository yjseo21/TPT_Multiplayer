# TPT multiplayer: eliminate video streaming with client-side simulation

**Status:** architectural design and implementation handoff, 31 October 2026. **Not implemented.** This document is a design grounded in the bundled `TPT-optimized-source-2026-10-31.zip`, not a claim of passing multiplayer tests. The existing authoritative server and WebP/video fallback must remain available throughout migration.

## Executive decision

Implement **server-ordered, locally simulated, deterministic lockstep**, with each player's native client rendering its own authoritative-equivalent world. Server still runs the complete simulation to validate the world. Only edits, tick commitments, cursors, state hashes, and exceptional checkpoint/recovery traffic traverse the network. This targets bandwidth proportional to user actions rather than particle count or 612x384 display frames.

It must be **gated** by reproducibility tests: if the clients cannot simulate bit-identical physics at every tick for all supported worlds/OS/CPU combinations, do not label the lockstep path reliable. Keep the current authoritative picture stream, and optionally develop render-only state-delta as an intermediate/fallback mode. Both are distinct from deterministic local simulation.

## 1. Three fundamentally different designs

| Design | Where simulation runs | What is sent continuously | Bandwidth consequence | Correctness |
|---|---|---|---|---|
| Existing streamed pixels (lossless/WebP) | Server only | Frame images / pixel deltas | Proportional to visual changes and viewer count | One authoritative physics world; visual artifacts if lossy |
| **Server simulates, clients render** (state streaming) | Server only | Particle type/position/velocity/life/temp and selected air/gravity/wall fields; diffs | Can be worse than image streaming for dense, rapidly moving scenes | Exact if complete lossless state and ordered updates; low-rate snapshots may be stale |
| **Server + every client simulate and render** (deterministic lockstep) | Server and all clients | Tick-assigned input events, hashes, occasional recovery checkpoints | Proportional to user edits, almost independent of scene complexity | Exact only if every deterministic step produces identical complete state |

The user's key proposal is excellent if understood as **the third design**. Simply moving the renderer to the client does **not** remove the need to transmit changing particle data.

### Concrete size comparison

- Canvas resolution: `XRES=612`, `YRES=384` = 235,008 pixels. Raw RGB frame: `705,024` B; sending 20 raw frames/s is ~112.8 Mbit/s per viewer (before compression).
- `Particle` is 14 32-bit fields (56 B in the source). A fully occupied particle array would have 235,008 x 56 = **13,160,448 B**, excluding air/gravity/electric maps and maps/metadata. Even a fraction of this state repeatedly sent can overwhelm a video stream. Actual compressed size is scene-dependent.
- Deterministic lockstep: illustrative four players x 20 edits/s x 80 encoded bytes/edit x 3 recipients = **19,200 B/s** or **0.154 Mbit/s aggregate outgoing from the server**, before networking overhead, tick seals, hashes, cursors, and acknowledgements. This is an illustrative traffic model, not a measurement. Per viewer the same event rate is ~51.2 kbit/s before overhead. Idle worlds can require far less.
- A checkpoint can still be megabytes. **Do not broadcast checkpoints periodically**: compute/retain one on server, send at join/reconnect/desync only. During normal gameplay, broadcast small hashes.

## 2. Existing source code we can reuse

This project already carries much of the engine in both its server and client binaries. Do not create a second physics engine.

- `src/multiplayer/World.{h,cpp}`: main-thread authoritative state; `World::Advance()` does `BeforeSim(true)`, `UpdateParticles(0, NPART)`, and `AfterSim()`. `World::Apply()` already normalizes most tool actions but applies them immediately at the current tick. **Change to tick-ordered command log.**
- `src/multiplayer/Protocol.{h,cpp}`: typed command parsing and a `TPM1` zlib state transport for `RenderableSimulation`. **This is not an exact resumable `Simulation` checkpoint**: missing RNG, full transient state, options, and some physics maps. It is useful for a rendering-only fallback prototype, not for lockstep initialization.
- `src/simulation/Editing.cpp` and `src/simulation/Snapshot.{h,cpp}`: `Simulation::CreateSnapshot()`, `Simulation::Restore()`, and `Snapshot::Hash()` already include most main arrays, particle storage, RNG state, and frame count. Reuse the fields and capture logic, not the current raw hash as a production consensus digest.
- `src/simulation/SnapshotDelta.{h,cpp}`: upstream undo/redo field-aware deltas. It stores old **and** new values and is tuned for in-memory history. Adapt it to forward-only network patches, revision IDs, bounded buffers, and zstd/zlib only for the fallback state-stream mode.
- `src/client/GameSave.{h,cpp}`: `ensureDeterminism`, `frameCount`, and `rngState` options, plus `World::Load()` restoring them. A CPS save may reorder particle slots or omit transient state, so **do not assume CPS is an exact multiplayer checkpoint**.
- `src/multiplayer/CanvasProducer.cpp`, `Canvas.cpp`, `Server.cpp`, and `Client.cpp`: retain as an operational fallback. Image output is *rendering output*, never an input to the physics world.
- `src/common/tpt-rand.h`: explicit two-u64 `RNG::State`; must be restored before replay and never mixed with renderer RNG.
- `tests/multiplayer/CoreTests.cpp`: existing same-process upstream-stepping test compares renderable state after 30 frames. It is a useful smoke test, **not cross-process or cross-platform determinism qualification**.

### Important hazards in the upstream snapshot abstraction

- `Snapshot::Hash()` is 32-bit FNV over object bytes; it excludes signs and Authors and can reflect structure layout or padding rather than a canonical serialized state. It also omits numerous flags/options necessary to make subsequent ticks equal.
- `Simulation::Restore()` resets `parts.active=NPART`, then rebuilds free slots with `RecalcFreeParticles(false)`. Verify that this produces the *same future allocation order* as the original, or store the private `Parts` free-list and active state explicitly. It also notes that Newtonian gravity enable is not stored.
- Engine-only mutable state outside `Snapshot`: audit `Simulation` fields such as `CGOL`, `GSPEED`, `gol`, `force_stacking_check`, element count/recount, transient transport buffers, air settings, gravity state, time-related counters, and options. Not all are necessarily necessary at tick boundaries, but they MUST be proven irrelevant or serialized.
- `RenderableSimulation` contains `pmap`, `photons`, and graphical/transient data not covered by ordinary snapshots, which must either be exactly rebuilt or included when necessary for future physics.
- Determinism also depends on execution order, numeric settings, runtime library math, compiler flags, `-ffast-math`/FMA, threading and gravity worker scheduling, element implementation differences, platform architecture, and optional Lua/mod callbacks. A synchronized RNG seed alone does not fix these.

## 3. Target architecture and flow

```text
Player A's local UI ── proposed edit ──▶ server command sequencer
                                            │
                    ┌───────────────────────┼───────────────────────┐
                    ▼                       ▼                       ▼
            Player A physics         Player B physics         Player C physics
            + local renderer         + local renderer         + local renderer
                    │                       │                       │
                    └────── state hashes ───┼───────── hashes ──────┘
                                            │
                                   server authoritative
                                   simulation + hash
                                            │
                               mismatch: exact checkpoint
                               then replay ordered edits
```

Normal play:
1. A sends a validated semantic edit, not per-particle mutations. Its UI may preview the brush **visually** until the server confirms an authoritative tick.
2. Server allocates a monotonically increasing global event ID and a precise `(tick, phase, order)` position, checks permission/rate limit, appends to log, and relays the event to all clients **including its author**. Client-side edits must not mutate physics ahead of authoritative sequence unless a rollback system is explicitly added.
3. Server publishes **tick commitment boundaries** (batch up to 10–30 times/s) so clients can advance through frames with no edits. At the committed tick boundary, each client applies all events in server order, advances its local `World` exactly once, and renders independently at the client's display refresh rate.
4. At configurable intervals (e.g., every 60 simulation ticks initially), every simulation submits a **canonical state digest for a specified tick**, not a hash of current visual pixels. The server compares it with its own digest. Matching digests cost only tens of bytes.
5. Missing events? Request replay by event ID. Mismatch? Stop applying new events for that client, load an authoritative **exact** checkpoint, replay subsequent ordered events, verify hash, and resume. If repeatedly mismatching, fall back to server-rendered stream for that client; never leave silently divergent worlds interactive.

### Tick and paused editing

Existing `World::Apply` mutates the simulation while `World::tick` remains unchanged; multiple paused edits are distinguished only by `World::revision`. A new timeline must preserve both concepts. Proposed event key `(epoch, tick, phase, ordinal)`:

- `phase=beforeStep` events are applied at the tick boundary before the physics step, ordered by `ordinal`.
- `phase=paused` edits are applied in sequence without advancing tick. A pause/unpause command is itself a sequenced event.
- Define exactly whether `Step` means apply pending edits before executing one physics tick (recommend yes).
- On reset/load, bump `epoch`; old event IDs and checksums from a previous epoch cannot be applied.

Do not base physics ordering on wall-clock arrival at clients. Packet arrival order is irrelevant if event sequence + tick are explicit.

### Network message sketch (design only)

Use version `protocol 4` or explicit opt-in capability negotiation; do not silently reinterpret protocol 3 (`TPC3` image stream) packets.

```
HELLO { protocol:4, engineHash, supportedDeterminismProfiles, mode:"lockstep", ... }
WELCOME { epoch, tickRate:60, physicsProfile, currentTick, ... }
CHECKPOINT_BEGIN { epoch, checkpointTick, eventId, bytes, digest, compression, chunkCount }
CHECKPOINT_CHUNK { epoch, index, payload, crc }
CHECKPOINT_END { epoch, checkpointTick, digest }
EVENT { epoch, eventId, tick, phase, ordinal, playerId, kind, canonicalArgs }
COMMIT { epoch, throughTick, lastEventId }
STATE_HASH { epoch, tick, hashAlgorithm, digest }
RESYNC_REQUEST { epoch, observedTick, expectedEventId, observedDigest }
REPLAY_EVENTS { fromEventId, ... }
```

Use bounded binary payloads for frequent messages, validated limits, and explicit little-endian layout. JSON is fine initially for low-rate event control. Authentication and authorization remain server-side. Never let a client submit its own `tick` as a trusted authority; clients request *actions*, the server chooses execution time. Send cosmetic cursors on a separate drop-eligible channel so they cannot stall physics events.

## 4. ExactCheckpointV1 design

An exact checkpoint is a deterministic restartable simulation state, not a screenshot, not `TPM1`/`RenderableSimulation`, and not an ordinary CPS file without demonstrated bit-exact restart. It must survive capture, serialization, transfer, restore, and future simulation ticks.

Versioned/checksummed header: `magic`, schema version, build/element hash, world epoch, tick, revision, latest event ID, active features, payload length, canonical digest. All fixed-width integral and float fields serialized with defined endianness and NaN/infinity rules. Strict upper bounds and CRC per chunk. Compression e.g. zstd if dependency is provisioned or zlib as a baseline.

Payload field registry: exact indexed particle slots (including free-list/next allocation behavior), RNG, frameCount, per-particle properties, air/heat/velocity/pressure arrays, walls/electric/air blocking, portal/wireless/stickmen/GoL state, gravity input/output/enable and concurrency barriers, simulation settings/options, and dynamic data that affects subsequent ticks. Include signs/UI data in a separate cosmetic section if not physics relevant, but require a complete scene for all clients. A serialized state audit should explicitly enumerate all `Simulation`/`Air`/`Gravity` mutable members as **stored / deterministically reconstructed / non-physics**.

Before every checkpoint, quiesce background workers and grab a coherent tick boundary. On restore, validate limits, atomically replace the world, rebuild derived indices, then verify canonical hash **before** resuming simulation. Test 1, 10, 100, 10k additional steps post-restore, not only immediate equivalence.

## 5. Determinism qualification gates

**Gate A — same binary, independent runs:** Two separate Linux processes, fixed `ExactCheckpointV1` and event log. Verify exact canonical state after ticks 1, 10, 60, 600, 10k. Test at least dust, dense water, FIRE, combustion/explosions, lasers, electricity, electronics, life, portals, SOAP, stickmen, pressure/heat and Newtonian gravity; include random elements and save/load in paused and running states.

**Gate B — exact checkpoint replay:** At a predetermined tick in a running world, capture into a third process, restore, apply identical future input, compare every subsequent digest with original. Confirm same particle ID/free-list allocation; no silent divergence when new particles spawn.

**Gate C — actual network:** Two-to-four clients, high concurrent editing, reordered/late/dropped control packets (WebSocket reconnection), bounded replay, 50/100/200 ms RTT and 1–3% losses. Fully verify event causal order while paused, stepping, reset, room admin changes, save/load, join/leave/reconnect, and one slow client.

**Gate D — cross-platform:** Windows x86-64 vs Linux x86-64 (and macOS/ARM if supported). Record compiler, CPU, FPU/SIMD mode, math library, enabled elements, mods and build SHA. Fail closed if unverified. An identical build hash doesn't imply floating-point equality across architectures.

Hash recommendations: test original `Snapshot::Hash()` for diagnostics, then replace networking hash with a canonical encoding that covers *all* future-physics-impacting fields and avoids padding. Use a strong 128-bit-or-larger digest for consensus; separate hash for cosmetic state if desired. On first mismatch log the first differing field and tick into a reproducible desync bundle (checkpoint + event log + build metadata) with no auth tokens.

## 6. Workload, latency and fairness

- Server must still run one authoritative 60 Hz simulation, **and** each client must have enough local CPU to run 60 simulation steps/s. On slow PCs the application should reduce rendering FPS before reducing physics ticks; a client that can't keep up must visibly enter catching-up/spectator mode rather than silently diverge.
- Server-to-client RTT is a usability concern. Assign edits to future ticks with a short input buffer, e.g. adjustable 3–12 ticks at 60 Hz depending on observed RTT/jitter; avoid hardcoded guarantees. The local UI can immediately show a *ghost* brush/cursor as latency hiding, but only server-ordered commands change physics.
- A persistent event log is needed for reconnect and audit; it can be periodically compacted after the server retains an exact checkpoint and enough rolling history.
- Server hashes are computed **at a named fixed tick**, not from concurrent worker snapshots or asynchronous mid-step state. GPU rendering differences are irrelevant if they do not feed back into simulation.
- Rate limiting, validation, moderation, room lock, TLS and admin permissions carry over. Only verified clients may simulate as authoritative-equivalent; anyone can be a streamed spectator. The server always wins on any collision/conflicting input.
- Exact synchronized state does not necessarily require synchronized display frame rates; 30/60/144 fps displays can all render the same 60 Hz simulation tick.

## 7. Alternative if full deterministic lockstep is not yet possible

**Renderer-only state streaming** can serve clients that support local rendering but not local physics. Build a separate server `SimulationStateEncoder` rather than send all `TPM1` state each update:

1. Partition particle slots and low-resolution air/wall fields into fixed-size chunks; attach generation/epoch/ack/baseline tick to each delta.
2. Send only changes: births/deaths, element changes, packed subpixel positions (lossless if needed), temperature/life and decoration updates; quantization requires an explicit **visual-only** quality mode, never feed approximate fields back into physics.
3. Keep IDs stable to avoid entire blocks changing on particle swaps. Compress distinct streams with entropy-appropriate coding (RLE for unchanged fields, bitsets of changed slots, zigzag/varint small differences, zstd/zlib across blocks). Batch deltas for a given view where practical.
4. Update slowly changing wall/air/gravity overlays independently of active particle fields. Renderer, zoom, HUD, fire/glow and color modes run entirely on viewer GPU/CPU, reducing per-view server encoder work and removing lossy image scaling.
5. Add periodic exact keyframes for the **renderable** state and bounded replay/resync, with per-client congestion control and memory limits. Benchmark dense liquids, fire, electronics and empty scenes against current WebP scale1/2/3 on **actual network bytes** and viewer CPU.

Caution: this alternative might improve zoom/visual quality and remove server rendering CPU, **but it cannot promise lower bandwidth than WebP**, especially when large numbers of particles change every tick. Do not disable the working image mode until measured.

**Hybrid prediction:** a client can run the local physics optimistically while accepting authoritative corrections. This saves bandwidth only when divergence is rare or localized; chaotic particle systems may spread small errors and force large patches. Implement after same-machine determinism tests, not before. Never mutate the server from predicted client state.

## 8. Implementation milestones, in order

1. **Determinism research instrumentation** — add a headless replay probe that accepts `--checkpoint` and `--event-log`, writes per-tick canonical hashes, compares two independent runs, emits the first offending field. Introduce fixed RNG seed at room creation and pin an engine/element build profile. Build/run with normal Meson CI first.
2. **ExactCheckpointV1** — schema, capture/restore on main thread, no changes to current network modes. Include engine settings audit and checkpoint-then-replay tests. Do not confuse CPS or `TPM1` with this.
3. **Command timeline** — introduce `(epoch,tick,phase,ordinal)` sequencing and persistent event journal. Reuse existing `ParseCommand`/`World::Apply`, but do not change when current visual-stream clients receive updates. Ensure paused edits and `Step` deterministic.
4. **Native local-simulation client** — initially a feature-gated Linux-only headless/client flag, using existing Simulation and Renderer. Join via exact checkpoint, commit events, catch up and render. Report local hash and tick backlog. First achieve 2-player same-host exact 10k ticks.
5. **Cross-platform matrix, recovery** — separate-process CI Linux/Windows, then macOS/ARM, selected determinism profile, resync, replay, fallback to video, clear failure UI. Do not claim tested configurations without evidence.
6. **Bandwidth/soak qualification** — four clients for 60 minutes on water/fire/dust/electronics; measure TCP/TLS wire bytes, checkpoint/replay volume, hashes, CPU, late ticks, client memory and reconnections. Targets: identical hashes for all verified clients, 0 silent desyncs, no continuing beyond uncommitted tick, and aggregate uplink dominated by commands under normal play.
7. **Optional render-only state delta** — use if heterogeneous-platform determinism is delayed. Benchmark it against optimized image stream rather than assume success.

## 9. Acceptance and operational metrics

Report per server/connection: `mode`, `engineProfile`, `serverTick`, `committedTick`, `clientTick`, `eventId`, `eventLagTicks`, `hashMatches`, `hashMismatches`, `checkpointBytes`, `checkpointCount`, `resyncTime`, `replayEvents`, `actualBytesSent`, `renderFps`, `physicsTickMs`, `clientCatchupTicks`. Long-duration tests should publish `.jsonl` plus charts.

**Hard release criteria:** complete tests for user-selected OS/CPU combinations, exact checkpoint replay for 10k ticks on selected scenes, zero silent divergence, server-side access control, recoverable client restart, bounded queue/bandwidth, and backwards-compatible video fallback. No feature should depend on a participant having to send their local simulated state to the server as proof of correctness; the server computes authoritative hashes itself.

## 10. Research/context

- Factorio's multiplayer employs deterministic lockstep and exchanges user inputs, not full world state: https://wiki.factorio.com/Desynchronization
- Glenn Fiedler's explanation of lockstep vs state sync: https://gafferongames.com/post/deterministic_lockstep/
- Floating-point reproducibility constraints: https://gafferongames.com/post/floating_point_determinism/
- State snapshot interpolation as an alternate design: https://www.gafferongames.com/post/snapshot_interpolation/

These references explain general networking techniques; they do **not** establish The Powder Toy's determinism. The qualification work above is mandatory before choosing this as default.
