# Deterministic multiplayer implementation — first executable milestone

**Date:** 2026-10-31. **Status:** development prototype; **not a playable local-simulation multiplayer mode**. This milestone implements an authoritative command timeline, optional command-commit broadcast, append-only deterministic-session journal, named state diagnostics, and a replay verifier. The existing video/canvas transport is unchanged by default.

Read this alongside `DETERMINISTIC_SIMULATION.md`, `README.multiplayer.md`, and `TPT/HANDOFF_2026-10-31.md`.

## 1. What is implemented

| Capability | Location | Notes |
| --- | --- | --- |
| Total edit ordering | `src/multiplayer/TickTimeline.{h,cpp}` | `eventId`, `epoch`, `tick`, `ordinal`, `afterTick`, `revision`, `resetGeneration`. Paused edits do not lose order. Invalid transitions fail. |
| Canonical edit payload | `src/multiplayer/CommandRecord.{h,cpp}` | Records **parsed, validated commands**, not arbitrary client input. Preserves shape, brush flags, element, tool, administrative pause/step/clear and defaults. Does not record room credentials. Replay validates commands again. |
| Optional ordered network commits | `src/multiplayer/Server.cpp` | A client can request `commandCommits:true` in `hello`. After successful application, authenticated opted-in peers receive a `command_commit` JSON message. Existing clients receive no additional event traffic. Does not imply that the client runs local physics. |
| Fixed-seed recording | `src/multiplayer/Journal.{h,cpp}` | `--seed N --journal FILE.jsonl` creates a journal of world-empty start, commits, initial/periodic/final state digests. The journal is flushed after each write. Server stops on commit-journal failure. `--load` and `--recover` are forbidden when recording. |
| Field-level state diagnostics | `src/multiplayer/StateDigest.{h,cpp}` | Endian-independent FNV-1a/64 diagnostics for clock/RNG, particle slots and free-list head, particle maps, air/current-history, walls, gravity, wireless/portals, players, settings, bookkeeping and GoL. Diagnostic only, not a production cryptographic consensus hash. |
| Independent journal verifier | `src/multiplayer/Replay.cpp` | `tpt-replay --journal FILE.jsonl [--digest-output FILE]` reproduces missing tick intervals, replays every edit in logged order, checks ordinals/revisions and reports **first mismatched named state group**; nonzero exit on divergence. |
| Standalone tests | `tests/multiplayer/timeline_standalone.cpp`, `command_record_standalone.cpp` | Built and passed with system GCC in the current container. No Meson required. |
| Build-integrated tests | `tests/multiplayer/CoreTests.cpp`, `replay_test.py`, `src/multiplayer/meson.build` | Added same-process short-replay assertions and a real-network journal/replay/tamper integration test. **Not executed** in this container because Meson is unavailable. |

### Wire format (opt-in)

An existing authorized `hello` message may include `"commandCommits":true`. The server replies with `"commandCommits":true` in `welcome` and broadcasts successful edits as:

```json
{"type":"command_commit","id":12,"epoch":0,"tick":100,"ordinal":2,"afterTick":100,"revision":24,"actor":2,"command":{"seq":8,"op":"brush","x1":100,"y1":40,"x2":120,"y2":40,"radius":2,"element":2}}
```

This example is schematic, **not a captured packet**. IDs and ordinal are server assigned. A `Step` command can increase `afterTick`; a `Clear` increments `epoch`. The existing per-sender `ack` still exists. No tick-commit fence, input delay, or exact checkpoint transport exists yet. Old clients are unaffected if they do not opt in.

### Why the journal is restricted to an empty world

Current CPS loading and `Simulation::Restore()` do **not** round-trip every future-physics-impacting state field. In particular, particle allocation order and private transient buffers can differ after a restore. Recording a session with an initial CPS and pretending replay is exact would hide these problems. Consequently `--journal` demands an explicit 31-bit seed and refuses `--load`/`--recover` until an actual `ExactCheckpointV1` is implemented and tested. Normal server loading and recovery continue to work without journaling.

The journal is *not* a persistent replay service or a resumable multiplayer save. It is intentionally a diagnostic development feature.

## 2. How to build on Linux

From `TPT/powdertoy` on a Linux development machine with Meson, Ninja, a C++20 compiler and the bundled dependency pack:

```bash
meson setup build-mp --buildtype=release \
  -Dbuild_multiplayer=true -Dstatic=prebuilt -Dlua=auto \
  -Dhttp=true -Dresolve_vcs_tag=no
meson compile -C build-mp
meson test -C build-mp --print-errorlogs \
  multiplayer-timeline multiplayer-core multiplayer-replay
```

Depending on the host, you may need to install Meson and development headers/libraries. The `libwebp` image codec remains optional and is unrelated to the command journal. If you have another configured Meson build directory, use it instead of replacing it.

Test the new timeline without a full build:

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror -Isrc \
  src/multiplayer/TickTimeline.cpp tests/multiplayer/timeline_standalone.cpp \
  -o /tmp/tpt-timeline-standalone
/tmp/tpt-timeline-standalone
```

Run a fixed-seed server and record a real session:

```bash
export TPT_ROOM_TOKEN='choose-a-long-room-secret'
export TPT_ADMIN_TOKEN='choose-a-different-admin-secret'
./build-mp/src/multiplayer/tpt-server \
  --paused --seed 12345 --journal ./session.jsonl
```

Start the native client from another terminal, issue commands, and stop the server **cleanly** to record the final check. Then run:

```bash
./build-mp/src/multiplayer/tpt-replay --journal ./session.jsonl \
  --digest-output ./verified-digests.jsonl
```

Do not publish secret tokens; the server reads them from environment variables. Journal paths should be protected against other users editing the file. Journal inputs are still validated; there is no cryptographic signature or provenance guarantee.

## 3. Verification performed in the present Linux container

| Test | Result | Scope |
| --- | --- | --- |
| TickTimeline standalone program | **PASS** | Multiple paused edits, step tick advance, epoch reset, gap, rejected rollback/revision/epoch/tick mistakes; GCC C++20 with `-Werror`. |
| CommandRecord standalone program | **PASS** | Brush, tool, pause, step and clear payloads; unsupported type rejection; linked with bundled jsoncpp, without the full engine. |
| Syntax check of six new/changed multiplayer translation units | **PASS** | GCC 14 `-std=c++20 -Wall -Wextra -Werror -fsyntax-only`; an approximate generated `ElementNumbers.h` was used only in a temporary outside-project include directory. |
| CoreTests source syntax | **PASS** | GCC C++20 normal syntax-only; an inherited mismatched `std::string` vs `shared_ptr` WebP assertion in CoreTests was corrected. Existing upstream unused-parameter warnings prevent imposing `-Werror` on all original test headers. |
| Python replay test syntax | **PASS** | `python3 -m py_compile`. |
| Full server/client recompile | **NOT RUN** | Meson absent; `pip install meson` failed because container DNS/network is disabled. |
| Live ordered-commit WebSocket test | **NOT RUN** | Requires newly built server. |
| Independent physics replay/tamper integration | **NOT RUN** | Requires newly built `tpt-replay`. |
| Windows/macOS/ARM and 10,000-tick qualification | **NOT RUN** | Future milestones. |

The old `linux-bin` executables included in previous deliverables are **not compiled from these sources** and do not support `--journal` or `command_commit`.

## 4. Guarantees, limitations, and threat model

- This is a main-thread ordered log of **accepted** world mutations. Server authentication, tool validation, rate limits, and current visual transport are unchanged. Only successful accepted player edits are logged; administrative room-lock and kick operations are not physics operations and are intentionally not part of the world journal.
- Physics ticks without edits are represented by gaps between event/check tick numbers. The replay tool steps through the gaps, rather than relying on clock timings.
- A `check` includes world `tick`, `revision`, `resetGeneration`, named state-group digests and a combined summary. It is emitted initially, every 60 physics ticks, and on orderly shutdown. This provides first differing **group**, not yet first differing particle/field/value, and checks may be up to 60 ticks apart.
- The digest is a diagnostic, **not a complete canonical serialization**. Some internal caches, additional mutable simulation variables and gravity worker details still need audit. Cross-process equal digests are useful evidence, **not sufficient proof** of deterministic evolution or checkpoint completeness.
- Different platform, compiler, optimizations, floating-point paths, threading and mod callbacks may diverge. Do not enable physics-on-clients in production until a comprehensive validation matrix passes.
- Without an exact, bit-preserving checkpoint, late join/reconnect cannot start local physics safely. Do not use `TPM1` render state or CPS as an exact checkpoint.
- `commandCommits` currently sends individual control JSON messages and is intended only for research clients. A high-rate production lockstep implementation should batch commands by tick and emit an explicit tick seal, hash and replay window. The existing per-peer control queue is bounded and may disconnect slow opt-in viewers.
- Logs can grow without bound over long sessions; rotating durable journals, bounded retention, auditing and restart/recovery are future work.

## 5. What the next engineer should implement

1. **Build and run the unexecuted tests** on a Meson-capable Linux builder. Diagnose any replay mismatch before adding networking complexity. Use `tests/multiplayer/replay_test.py` to cover two separate verifier processes, pause/step/clear ordering and tamper detection.
2. **ExactCheckpointV1**, versioned binary schema: capture at quiescent tick boundaries; encode every future-physics-affecting field, particle slot/free-list head/links, engine options, RNG, air history, gravity worker state and specific element internal buffers. Strict little-endian encoding, bounded lengths, checksum and strong digest. Restore then compare at tick 0, 1, 10, 100, 10,000, including future particle allocations.
3. **Full field audit and canonical cryptographic hash** (e.g., SHA-256/BLAKE3) over exact serialized data. FNV-64 exists solely to localize problems, not defend against adversarial participants or accidental hash collisions.
4. **Deterministic cross-process test harness**, with reproducible scenes and an input log generator; verify normal GCC/Clang, Linux/Windows and differing x86-64/ARM floating-point behaviors. First divergence should produce a minimal repro bundle (checkpoint, ordered inputs, hashes, build metadata; never credentials).
5. **Actual locally simulated spectator/client:** start in a supported same-build profile, load exact checkpoint, process server-committed tick batches, locally call `World::Apply` and `Advance`, compute hashes, then render with the existing `Renderer` on the client; keep image fallback for unsupported/mismatched configurations.
6. **Synchronization and recovery:** future-tick input delay, explicit tick seals, bounded event history, checkpoint transfer and replay, intentional rollback/catch-up or safe fallback; administrative commands, lifecycle and reconnect tests; 4-client high-load/soak bandwidth and CPU measurements.

### Bandwidth design decision

Server ordering + local physics is still the preferred target because normal traffic is edit messages and small periodic hashes rather than per-frame pixels or per-particle state changes. **It has no verified Mbps improvement in this milestone**: the shipping client still uses the previous lossless/WebP visual transport. First prove physics reproducibility and checkpoint correctness; then measure actual bytes across four clients with local physics enabled.
