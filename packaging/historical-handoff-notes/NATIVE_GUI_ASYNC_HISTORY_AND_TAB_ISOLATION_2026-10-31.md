# Native TPT multiplayer continuation: asynchronous history, deterministic import, tab isolation

Date: 2026-10-31

Source: `/mnt/data/tpt-work/TPT/powdertoy/`. This is an implementation and testing handoff, **not** a claim that all original-TPT GUI compatibility milestones are complete. The original 612×384 particle grid is unchanged. Packaging was intentionally deferred.

## Problems addressed

1. **History compression stalls.** `World::Apply(history_mark)` previously serialized **and zlib-compressed** a complete ExactCheckpointV1 on the authoritative physics thread. Undo and redo did likewise. This could freeze simulation/input handling for a compressed full-world snapshot.
2. **Legacy save nondeterminism.** `World::ReplaceFromSave` created a fresh `Simulation` with a random seed. Typical upstream saves (`ensureDeterminism=false`) lack serialized RNG state. Two independent replicas loading identical OPS data could therefore start with different random states; this was also seen as intermittent equality-test failures in the original regression.
3. **Singleplayer tab stale world.** When single-player `GameModel` replaced its `Simulation` instance (e.g., loading a save), its persistent tab reference was not refreshed before entering Multiplayer. Switching back could restore the old private world instead of the current one.

## Implementation

### Split exact checkpoint capture and compression

`src/multiplayer/ExactCheckpoint.h/.cpp` now exposes:

- `CaptureExactCheckpointRaw(World&)`: owner-thread-only, exact serialization to a bounded immutable byte string. It must run between completed edits/physics ticks.
- `CompressExactCheckpointRaw(std::string_view)`: pure zlib compression, CRC and unchanged TPTXCP1! envelope. It has **no access to the simulation** and may run on any worker.
- `EncodeExactCheckpoint(World&)`: existing public interface, now composing both functions. The wire format, CRC validation, transactional restore behavior and compression algorithm remain unchanged.

### Bounded history compression worker

New files `src/multiplayer/HistoryCompressor.h/.cpp` implement one *lazily created* background thread per history-enabled authoritative `World`. Snapshots are owner-thread captured and submitted as immutable raw strings to a bounded FIFO. `std::promise`/`std::future` connects each compression result to its history entry. There is **no separate thread spawned for every gesture**. Pending raw bytes are limited to **128 MiB**, and attempts to exceed the bound backpressure the submitter rather than exhaust memory. Destruction joins the worker and does not leak tasks. The worker never reads the mutable simulation or GUI state.

`src/multiplayer/World.h/.cpp` now maintains pending/compressed history entries, resolves completed futures opportunistically, and waits for an unfinished result only when a user requests undo/redo immediately after marking. Undo restores a fully verified exact checkpoint; the authoritative tick/revision handling and replica rebase remain unchanged. History bounds are 16 entries, a 32 MiB compressed target and a conservative 128 MiB reservation while raw snapshots are pending. **Capturing raw exact state and applying a restore still happen on the simulation thread**; the refactor removes compression latency, not all history-operation latency. If a script submits snapshots faster than compression can consume them, bounded backpressure may still be felt.

The class is deliberately separate from `World.cpp` for further testing and profiler instrumentation. Its translation unit is included in `src/multiplayer/meson.build` and in the temporary Linux `/mnt/data/tpt-work/CMakeLists.txt` development harness. The supported Meson production build was **not** run in this session.

### Deterministic fallback for upstream OPS save files

`World::ReplaceFromSave` now uses an explicit FNV-1a 64-bit seed derived from canonical serialized save bytes **before** loading particle data. If the OPS file carries an RNG state, that state still takes precedence after loading. This makes the missing-RNG case reproducible among build-compatible local simulations; it is not intended to preserve an unspecified original random seed. The authoritative and replica simulations must still use the same compatible engine build and element implementation.

### Preserve the current single-player simulation instance

`GameController::SelectEditorTab(true)` now refreshes `singleplayerSimulation = gameModel->ShareSimulation()` before adopting the multiplayer world. This preserves the private world when `GameModel` has replaced its simulation since the previous tab switch. The original clipboard remains application-wide, allowing Singleplayer copy → Multiplayer paste. Full per-tab save metadata, Lua state and undo-history independence still require additional design and tests.

### Restore original online-save upload as a non-mutating action

`GameController::OpenSaveWindow` now retains the original `ServerSaveActivity` dialog while in Multiplayer. It captures a **copy** of the client’s verified shared simulation and posts a new online save through the normal upstream UI, with the current shared pause state. The completion callback only presents an information tip: unlike the stock singleplayer callback, it does **not** invoke `LoadSave` against the authoritative room. `SaveAsCurrent` in the multiplayer tab also redirects to a new upload, preventing a user from unintentionally overwriting an unrelated private tab save ID. Authentication is still handled by upstream TPT; no credentials are exchanged with the room server for uploads.

This change **compiled in the native Linux GUI and passed the existing cross-tab clipboard regression**, but a real account-backed upload to the upstream online service was not possible in the container. Visual dialog QA, error handling when the upstream service is down, and cloud save metadata in two tabs must still be tested manually. No cloud upload should be claimed verified from this build alone.

### Physics build compatibility bump

The deterministic fallback seed changes the physics outcome for older OPS saves lacking RNG state. Therefore `src/multiplayer/Protocol.h` and the network test fixture now use **`tpt-native-gui-parity-20261031-v9`**, not v8. All server, native GUI, replay and replica binaries must be rebuilt together. Existing v8 peers must be rejected at handshake rather than allowed to silently diverge when importing a legacy save.

## Test evidence

- CMake/Ninja scratch rebuild: `tpt-server`, `tpt-replica`, `tpt-replay`, `tpt-original-gui-multiplayer`, `tpt-paste-tests`, `tpt-checkpoint-inspect`, `settings_core`, `checkpoint_core`, `replica_core`: compiled.
- GCC and Clang independently accepted `HistoryCompressor.cpp` with `-Wall -Wextra -Werror -fsyntax-only`.
- Core settings, exact checkpoint and replica-engine tests: passed.
- Extended `tpt-paste-tests`: passed including serial raw/compressed byte identity, immediate and repeated history undo/redo with compression pending, memory/entry checks, shared save import, decoration and sign conflict. A repeat invocation completed in approximately 20 seconds on this unoptimized Linux development build; **this is not a history latency benchmark**.
- `native_gui_parity_network_test.py`: passed (15 accepted live network edits, two equal outputs).
- `native_history_journal_test.py`: passed (two journal checks, six ordered edits).
- `native_history_network_test.py`: passed (admin checks, full checkpoint resynchronization with an independent local replica).
- `native_gui_four_player_xvfb.py`: passed (four original GUI windows, synchronized editing, **zero canvas video frames** in the short smoke test).
- `native_gui_tab_clipboard_xvfb.py`: passed (real SDL/Xvfb Singleplayer → copy → Multiplayer → paste; authoritative checkpoint showed **204 particles and revision 1**).
- Full umbrella test script was started but its execution was interrupted by the external command timeout after the engine/paste section. Do not report it as a complete green acceptance run; the network and GUI tests above were then executed individually.

## Rebuild and continue

```bash
cmake -S /mnt/data/tpt-work -B /mnt/data/tpt-work/cmake-build -G Ninja
cmake --build /mnt/data/tpt-work/cmake-build --target \
  tpt-server tpt-replica tpt-replay tpt-original-gui-multiplayer \
  tpt-paste-tests tpt-checkpoint-inspect settings_core replica_core checkpoint_core -j4

# Developer-level unit and integration checks
/mnt/data/tpt-work/cmake-build/tpt-paste-tests
cd /mnt/data/tpt-work/TPT/powdertoy
python3 tests/multiplayer/native_history_network_test.py \
  /mnt/data/tpt-work/cmake-build/tpt-server /mnt/data/tpt-work/cmake-build/tpt-replica
```

The GUI tests require Xvfb and the Python Xlib/XTEST module. In a native desktop start the original GUI with `tpt-original-gui-multiplayer disable-network`, click Multiplayer, and supply a WSS URL or local loopback WS URL. Use separate room/admin tokens. See `docs/multiplayer/NATIVE_GUI_EDITOR_TABS_AND_PASTE_2026-10-31.md` for the original tab workflow.

## Remaining original TPT parity blockers and engineering priorities

1. **Finish native GUI per-tab state isolation**: history state, save identity, Lua/console context, and undo isolation beyond the private simulation pointer. Regression-test save replacement while switching tabs.
2. **Provide genuinely multiplayer-safe Lua functionality** via a server-controlled operation layer and deterministic replay, not unrestricted client mutation of authoritative physics.
3. **Permit OPS stamps/saves beyond the existing 512 KiB command limit** using checked chunked uploads and bounded server-side assembly; preserve atomic publish and replay.
4. **Advance shared history semantics** from administrator-only global undo to concurrency-safe per-author operations that cannot erase subsequent edits from other people.
5. **Reduce remaining dense-world pauses**: profile raw state capture/restore and checkpoint bootstrap; add bounded asynchronous copy-on-write capture if achievable without race or determinism changes. Current native replica physics still runs on the GUI thread.
6. **Complete exact upstream UI behavior audit** for all element, wall, decoration, property, sign, stamp, inspector, render, clipboard, save/upload, browser, script and shortcut combinations.
7. **Qualify real platforms** with Meson release builds, original GUI screenshots, Windows/macOS, cross-compiler behavior, WAN jitter/loss and extended four-player high-density sessions. None of these are established by these short Linux tests.

Implementation invariants: command-only simulation in compatible rooms; no client-only authoritative physics mutations; no routine video streaming; exact checkpoints on recovery; raw history capture only at quiescent owner-thread boundaries; no unbounded receive, upload or history buffers. No engine particle-capacity enlargement.
