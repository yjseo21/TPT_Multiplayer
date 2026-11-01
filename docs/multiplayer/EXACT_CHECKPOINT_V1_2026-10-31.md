# ExactCheckpointV1 — implementation milestone (2026-10-31)

**Status: EXPERIMENTAL SOURCE IMPLEMENTATION, NOT A RELEASE.** The source was syntax-checked on Linux/GCC 14, but the runtime tests have not executed because this container has no Meson and cannot install it from its configured package index. Do not advertise exact restore, WAN synchronization, or local physics as verified. The original server/client binaries are older than this code. Begin here and in `LOCKSTEP_SPIKE_2026-10-31.md`.

## Purpose and scope

The upstream `Simulation::CreateSnapshot()/Restore()` is built for visual undo, and reconstructs particle allocation with `RecalcFreeParticles()`. That **changes free-list order** and can cause replay divergence after a future allocation. `ExactCheckpointV1` instead encodes the transient engine state bit-for-bit into an endian-stable, zlib-compressed binary record. It is intended as the eventual join/reconnect bootstrap for replicated local simulations. This milestone provides **offline capture/restore and server shutdown/startup flags**, not checkpoint broadcast or client simulation.

### Source files

- `src/multiplayer/ExactCheckpoint.{h,cpp}` — `EncodeExactCheckpoint(World&)` and `RestoreExactCheckpoint(World&,std::string_view)`; bounded, versioned binary codec; CRC32 and strict decompression length/stream-end validation; decode in a separate `World` before swapping into the destination.
- `src/simulation/Simulation.h`, `Simulation.cpp` — `Parts::RestoreExactFreeList()` validates and reinstates particle allocator order, including cycle/orphan detection. No change to the default `CreateSnapshot()/Restore()` path.
- `src/multiplayer/StateDigest.cpp` — additional diagnostics for transient caches and miscellaneous fields. These are 64-bit non-cryptographic group hashes, **not** a SHA-256 state signature.
- `src/multiplayer/Server.cpp` — optional `--exact-load` and `--exact-save` flags. Save is atomic at **clean shutdown only**. Checkpoint files are not CPS, cannot be opened by upstream TPT, and are not currently autosaved.
- `tests/multiplayer/CheckpointTests.cpp` — unit/integration test expecting bit-exact restore, particle free-list behavior, 120 subsequent independent ticks, rejection of truncation, corruption, appended data, invalid version/length, and safe original-state preservation.
- `tests/multiplayer/checkpoint_server_test.py` — actual server restart test, exact file comparison, rejection of corrupted file and conflicting CLI flags.
- `src/multiplayer/meson.build` — compile/link the codec and register both tests.

## Binary format

Single record, little-endian, no raw C++ struct dumps (so compiler padding does not enter the stream):

| Field | Bytes | Notes |
|---|---:|---|
| Magic | 8 | ASCII `TPTXCP1!` |
| Outer version | 4 | 1 |
| Raw state size | 4 | Strict bound: 0 < size <= 128 MiB |
| CRC32 | 4 | Over decompressed state, **error detection only** |
| Payload | variable | One zlib stream; trailing bytes are rejected |

The full packet is capped at **64 MiB**; high-entropy or near-max particle saves could exceed this and be rejected. The raw state begins with an internal version, resolution, particle capacity, channel count, and the `BuildId` bytes. The remaining strictly ordered fields include:

- World tick, revision, reset generation, pause state.
- Simulation tick/frame counters, RNG 128-bit state, **all particle slots** (including inactive trailing slots) and **exact free-list head** and active span.
- Pixel occupancy maps, `pmap_count`, current and historical air/heat/velocity fields, walls, air-blocking masks, fan fields, air kernel, and inactive gravity field arrays.
- Portal slots, wireless channels, stickmen/fighters, GoL buffers, LOLZ/LOVE/PSTN/PPIP scratch arrays.
- Element counts and flags, electrical/EMP bookkeeping, settings, air options, sand color phase, decoration, and up to 16 signs (Unicode code points, position, justification).

Scalar int/float values are encoded as 32-bit little-endian *bits*; uint64 values are encoded as 64-bit little-endian. `std::bit_cast` preserves NaN payloads and signed float zero. No host alignment, compiler ABI padding, or native `size_t` values appear on the wire. The packet's version checks dimensions and a **declared BuildId**, not compiler flags, CPU architecture, or a full executable digest. Compatibility with a different build is **not established**.

### Explicitly unsupported

- Active Newtonian gravity worker (`sim.grav != nullptr`) and active Lua callbacks (`useLuaCallbacks`). Snapshotting concurrently executing gravity threads is unsafe; `EnableNewtonianGravity()` is not used during restoration. These modes raise an exception instead of silently producing a lossy checkpoint.
- Capture while a particle update is in flight or concurrent mutations occur. Both APIs are **world-owner-thread only** and intended to run at quiescent step boundaries.
- Full portable cross-platform determinism. Equal bytes at time zero do not guarantee equal future physics with different FMA, libm, compiler optimization, multithreading or mod implementations.
- Cryptographic integrity/authenticity. CRC32 is not resistant to malicious manipulation. Server files are trusted-local only until a separately authenticated transport is built.
- Verified coverage of any future upstream `Simulation` fields or hidden static element state. New engine data must be added to this schema and its tests. Do not treat this as automatically complete after upstream code changes.
- Journaling sessions initialized from checkpoints. The existing `--journal` mode remains intentionally **empty-world-only**; `--exact-load` cannot combine with `--seed`, `--journal`, `--load`, or `--recover`.

## How to build and use

With an x86-64 Linux builder, Meson, Ninja, C++20, and bundled prebuilt dependencies:

```bash
cd TPT/powdertoy
meson setup build-mp --buildtype=release \
    -Dbuild_multiplayer=true -Dstatic=prebuilt -Dlua=auto \
    -Dhttp=true -Dresolve_vcs_tag=no
meson compile -C build-mp
meson test -C build-mp --print-errorlogs \
    multiplayer-checkpoint multiplayer-exact-checkpoint-server \
    multiplayer-core multiplayer-replay
```

Then run the experimental server. Use loopback, or correctly configure credentials and TLS for remote access:

```bash
export TPT_ROOM_TOKEN='your-room-secret'
export TPT_ADMIN_TOKEN='your-admin-secret'
./build-mp/src/multiplayer/tpt-server --paused --seed 12345 \
    --exact-save ./checkpoint.tptx
# Press Ctrl+C cleanly to write the checkpoint; abrupt termination does NOT save it.
./build-mp/src/multiplayer/tpt-server --exact-load ./checkpoint.tptx \
    --exact-save ./checkpoint-after-restart.tptx
```

For reproducible tests, use `--run-for 1 --paused` to have the server quit itself. Do not combine the checkpoint options with incompatible CPS loading/recovery commands. A newly loaded checkpoint preserves its own pause state, current tick, revision and RNG state. If both `--paused` and `--exact-load` are supplied, `--paused` explicitly overrides the restored pause state.

## Verification matrix: what actually ran in this container

| Check | Status |
|---|---|
| GCC 14 C++20 strict syntax for `ExactCheckpoint.cpp`, `CheckpointTests.cpp`, `Server.cpp`, `StateDigest.cpp` | **PASS** |
| GCC C++20 syntax for modified upstream `Simulation.cpp` | **PASS**, with upstream warnings not promoted to errors |
| Fresh build and run of timeline standalone test | **PASS** |
| Previous build's command-record standalone test | **PASS**, unchanged source under this milestone |
| Python syntax for new server integration test | **PASS** |
| Full Meson build of modified server/test binaries | **NOT RUN** — Meson unavailable |
| Exact checkpoint 120-tick runtime test | **NOT RUN** |
| Live CLI checkpoint save/load test | **NOT RUN** |
| Independent-process 10,000-tick replay across platforms | **NOT RUN** |
| Multiplayer clients running local physics | **NOT IMPLEMENTED** |

Upstream `Simulation.cpp` generates warnings under `-Werror` because of existing unused arguments and incomplete aggregate initializers; the new codec/test/modified server units passed with `-Werror`. The generated `ToolNumbers.h` used for syntax-checking upstream simulation code was created in the **external scratch directory**, not shipped as a build artifact. Meson generates its real version on a complete build.

## Immediate next engineering actions

1. Install Meson on a working Linux builder and run the **new** test cases; fix any failures before trying a network checkpoint. In particular, investigate whether two live Worlds can advance 120 ticks without hidden global state interference.
2. Repeat checkpoint exact-byte comparisons after **10,000 ticks** in two processes, with a comprehensive scene matrix (water, fire, air, electronics, GoL, pipe, portal, stasis, stickmen, multiple allocations, deletes). Save binary diffs and first divergent field. Use ASAN/UBSAN, corruption fuzzing, and malformed zlib inputs.
3. Audit persistent simulation state against every mutable field and static global. Bump internal schema version on incompatible changes; replace the fixed `BuildId` gate with a verified exact engine/build fingerprint. Improve decoder semantic validation before accepting packets from outside trusted disk storage.
4. Design a **bounded, authenticated, chunked checkpoint transfer** protocol for late-join clients with rate limits, SHA-256, cancellation, and generation/tick metadata. A 64 MiB checkpoint cannot simply be inserted into the current WebSocket JSON control queue; the current canvas control queue is deliberately size-limited.
5. Implement a **tick seal / command batch** stream, deterministic input delay, local physics renderer and per-tick hash reports. Clients must not advance beyond an unsealed tick. During hash mismatch, freeze or fall back to visual frames and request checkpoint plus missed command journal.
6. Measure actual compressed checkpoint sizes and post-bootstrap command traffic from varied real playgrounds, including worst-case high-entropy worlds, four users, slow WAN and packet loss. Do not claim bandwidth feasibility solely from codec results.

## Bandwidth model

The checkpoint is a **one-time or occasional** large transfer. After validated lockstep works, normal traffic should be **batched semantic edits and periodic hashes**, not video. A busy playground will then increase each computer's **CPU/GPU** requirement rather than its per-tick network bandwidth. Until the missing tick-sealed client implementation and 10,000-tick determinism gates are met, the shipping system still streams optimized frames; there is **no measured gameplay bandwidth improvement in this checkpoint milestone**.
