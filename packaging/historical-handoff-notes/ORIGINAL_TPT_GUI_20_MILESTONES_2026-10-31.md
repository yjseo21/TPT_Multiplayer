# Original The Powder Toy GUI + authoritative multiplayer: 20-milestone engineering handoff

Date: 2026-10-31. Experimental Linux source, **not a parity-complete release**.

## Product contract

Preserve upstream The Powder Toy 100.1.401 SDL/UI source, layout, event handlers, tools, menus,
save browser, zoom/HUD and keyboard behavior. Add a multiplayer connection path to the original app,
not a substitute recreation of its UI. In a shared session only the server orders world mutations.
Client physics advances exclusively via checkpoint + command commits + server tick seals. All players
render their own physics state and the server transmits no routine video to local simulation peers.
Purely local appearance controls need no network messages. **A visible upstream widget is not evidence
its mutation is multiplayer-safe.** Unsupported actions must never silently mutate a local replica.

**Critical recovery warning:** The worktree was restored from an older `TPT-interactive-local-physics-source-2026-10-31.zip`.
Milestones 5–17 from earlier conversations are partially documentary, not necessarily source-complete.
The implementation here is based on that recovered tree plus the gameplay feature milestone; do not
claim it includes every later improvement.

## Status for the 20 GUI parity milestones

The status below is deliberately graded. `Native` means upstream widgets/code are present, not that
all interactions are synchronized; `Verified` means tested in this Linux container.

| # | Compatibility category | Current evidence / remaining delta | Status |
|---|---|---|---|
| 1 | Exact original desktop window, layout, native menus | Stock GUI compiled with original `GameView`/`GameController` and launched under Xvfb | Native verified |
| 2 | Multiplayer session bootstrapping | Native `multiplayer=...` launch, authenticated WebSocket hello, exact checkpoint + seals, no server video frames | Verified on Linux |
| 3 | Original element menus and search | Uses upstream `GameModel::BuildMenus` rather than custom palette; element search widgets inherited | Native, partial E2E |
| 4 | Ellipse, rectangle, triangle brushes | Wire preserves shape and independent X/Y radii; core regression | Implemented/tested, 0–16 radius cap |
| 5 | Draw point/drag/line/rectangle/fill | Original event methods route ordinary tool commands; 18–20 edits from real Xvfb mouse | Implemented/tested for basic drawing |
| 6 | Original elements, wall and simulation tools | Server executes `Tool::Draw*`; upstream wall/fan regression | Partial; some UI-only tools require adapters |
| 7 | Deco palette, blending, color and geometry | 7 original modes, RGBA, point/line/box/brush server commands; network comparison passes | Implemented except renderer-dependent deco flood fill |
| 8 | Property editor and paint | Original dialog retained, property value/scale/shape ordered to server, paint/line/box/fill replay | Implemented/engine-tested, broader UI QA open |
| 9 | Signs: add, edit, move, delete, interactive buttons | Original `SignWindow` sends sign commands; roundtrip/replay tests, live network tests | Implemented/partly UI-tested |
| 10 | Original physics/options dialog | 15 options routed through server; display settings remain local | Implemented; settings-dialog click-through QA open |
| 11 | Pause, step, clear, gravity/air/reset shortcuts | Commands server ordered, resets and shortcuts implemented; permission checks | Implemented/tested on engine and network |
| 12 | Eyedropper, inspector, sampling | Upstream sampling UI remains; server inspection supported separately in older custom client | Partial; full stock inspector interactions unverified |
| 13 | Copy and cut selection | Original local clipboard copy; cut issues an authoritative area-clear command | Partial; cut integration and exact edges need QA |
| 14 | Stamps, paste, rotations and transformed placement | Original widgets remain; paste mutation intentionally blocked while authoritative transport absent | Not multiplayer-enabled |
| 15 | Undo/redo, full history fidelity | Original history UI exists; restore/forward blocked to prevent client-only divergent snapshots | Not multiplayer-enabled |
| 16 | Local saves, cloud saves, search and previews | Upstream dialogs intact; some loading/upload paths deliberately blocked | Partial, unsafe paths blocked |
| 17 | Lua console, scripts and user elements | Original Lua source compiled, but local simulation mutation commands blocked | Not multiplayer-enabled; deterministic Lua command replay needed |
| 18 | Render modes, zoom, HUD, fullscreen, screenshots | Stock original renderer and GUI code inherited; independent visual preferences | Native/inherited; full interaction QA open |
| 19 | Lobby, roster, cursors, chat, moderation in stock UI | Server protocol/features exist and old SDL UI uses them, but native `GameView` overlay not implemented | Not native-integrated |
| 20 | Complete parity, four-player soak, native Linux/Windows/macOS release | Linux scratch build + short Xvfb/network tests only; Meson and OS matrix outstanding | Not complete |

## New source changes this continuation

- `src/multiplayer/NativeSession.h/.cpp`: bounded WebSocket transport, replica checkpoint/tick-seal consumer,
  authenticated hello, original GUI simulation adoption, command sender, sign/property/decor/setting support,
  resync and error status, strict local-only plaintext WebSocket restriction.
- `src/multiplayer/World.h`: shared simulation ownership with upstream native GameModel.
- `src/gui/game/GameModel.h/.cpp`: `AdoptReplicatedSimulation()` for authoritative replica world.
- `src/gui/game/GameController.h/.cpp`: original UI connections, action interception, no independent
  `Update()` physics step, sign buttons, resets, native options, local replace-mode flags, blocked unsafe paths.
- `src/gui/game/tool/SignTool.cpp`: original sign modal sends ordered add/update/delete/move commands.
- `src/gui/options/OptionsModel.cpp`: sends physics-changing options to server instead of mutating world.
- `src/PowderToy.cpp`: original executable command-line `multiplayer=URL`, `multiplayer-name=NAME`,
  `multiplayer-ca=CAFILE`; URL parsing fixed for colon-containing scheme.
- `src/multiplayer/Protocol.h/.cpp`, `CommandRecord.cpp`, `World.cpp`, `Tools.cpp`:
  versioned command validation and canonical journaling, native brush shape/size, decorations,
  property values, signs, resets, sign activation and rectangle clear.
- `meson.build`: native GUI adapter included under `-Dbuild_multiplayer=true -Dbuild_powder=true`.
- `tests/multiplayer/CoreTests.cpp`: deterministic command roundtrips, property/deco/sign replay and malformed input.
- `tests/multiplayer/native_gui_parity_network_test.py`: real WebSocket multi-user operations with two equal final frames.
- `tests/multiplayer/network_test.py`: adjusts BuildId test fixture.

**Build compatibility:** `BuildId` is now `tpt-original-native-gui-20261031-v1`; do not connect older
binary clients with an earlier incompatible action schema. Additive protocol operations use protocol 3.
Update all client, server and replica executables together. This is not a formal stable release API.

## Reproduce the verified Linux build

The test container does not have Meson, so a temporary CMake harness outside the source root was
used to compile the original GUI and server:

```bash
cd /mnt/data/tpt-work
ninja -C cmake-build -j 4 tpt-server tpt-original-gui-multiplayer tpt-replica settings_core replica_core
./cmake-build/settings_core
./cmake-build/replica_core
python3 TPT/powdertoy/tests/multiplayer/native_gui_parity_network_test.py \
  ./cmake-build/tpt-server ./cmake-build/tpt-original-gui-multiplayer
python3 native-gui-smoke/input_test.py  # requires Xvfb, python-xlib, XTEST
```

Manual native UI, two terminals (in an X11/Wayland desktop or Xvfb):

```bash
export TPT_ROOM_TOKEN=testing
export TPT_ADMIN_TOKEN=admin
./cmake-build/tpt-server --tick-rate 30
# new terminal with same environment variables
./cmake-build/tpt-original-gui-multiplayer 'multiplayer=ws://127.0.0.1:34403' \
  'multiplayer-name=NativePlayer' disable-network
```

The non-production CMake harness is `/mnt/data/tpt-work/CMakeLists.txt` and generated resources
are under `/mnt/data/tpt-work/build-headers`. **A production Meson build has not been run** and
no Linux release packaging has been produced. Compile with GCC or Clang to identify macro and
platform-specific errors. Do not promote the scratch executable as a supported release build.

## Verified in this session

1. `settings_core`: PASS (brush-shape codec, sign CRUD/replay, decoration color/geometry,
   property temperature conversion, resets, sign button, options, original core tests).
2. `replica_core`: PASS (exact bootstrap, 120 ticks, edit ordering, digest failure and recovery).
3. `native_gui_parity_network_test.py`: PASS, two client sockets, 14 accepted actions, admin
   checks, malformed action rejection, identical rendered pixels from both server viewers.
4. Original executable under Xvfb with injected XTEST mouse drawing: PASS; server accepted
   18–20 authoritative revisions, 1 native client, 0 server video frames, near-30 Hz server ticks.
5. Scratch Linux GUI, server and headless replica CMake builds: PASS.

These are focused tests. They do **not** establish that all tools, UI dialogs, Lua scripts, saves,
plugins and cross-compiler/platform builds are compatible.

## Known correctness blockers and next ordered tasks

1. **Shared save/clipboard/stamp import:** implement bounded binary GameSave upload, server parsing and
   transactionally applying, commit/replay snapshots, and immutable chunk hashes before enabling
   original paste, save-preview-load, cloud/local load, transformations and stamp placement.
2. **Undo/redo:** server-managed authoritative history with permission/ownership rules, checkpoint
   reconstruction and full event log consistency; never allow client-local `Simulation::Restore()`.
3. **Lua:** separate client-side visual scripting from authoritative simulation mutations; compile
   command-only Lua operations in deterministic server order with strict resource limits.
4. **Decoration fill:** original renderer-dependent flood-fill needs a deterministic server-side
   renderer frame or a safe region-mask payload, verified against native upstream behavior.
5. **Options:** validate UI rendering on click-through, ensure all direct GameModel setters and plugins
   are either local or server-ordered; Newtonian gravity background state remains incompatible.
6. **Large brushes:** current 16-cell radius wire cap does not match upstream unlimited brush sizes.
   Chunk large gestures without changing per-stroke shape or application order, and rate-limit work.
7. **Sign ordering:** sign indices can change concurrently when multiple players edit/delete; add
   stable sign identifiers or conflict/version checks to match expected collaborative behavior.
8. **Native presence:** integrate the existing room roster/chat/cursor protocol into an overlay in
   original GameView without changing its default single-player layout.
9. **Input latency/CPU:** dedicated native replica worker, interpolation of *cosmetic* cursor only,
   local command preview with rollback on server rejection, source-compatible frame scheduling.
10. **Platform tests:** Meson release build, native Windows/macOS builds, GCC/Clang equivalence,
    four-player 60-minute dense-world soak, simulated and real packet loss, profile hot spots.

### Safety/behavioral boundaries

- No player simulates authoritative physics independently from server tick seals.
- Commands are validated and sequenced; sign/decoration/property messages are not rendered video.
- Shared menus exposing unsupported operations should show clear status, not silently mutate.
- Currently local renderer and native GUI main thread also process replica physics; slow worlds
  can freeze interaction. This is NOT yet at the single-player smoothness target for all scenes.
- Current protocol has no prediction for local brush edits; at WAN latency input may lag. Optimize
  without sacrificing deterministic command ordering or applying unacknowledged physics locally.
- Existing video fallback and later resume/async-source milestones must be reconciled from their
  actual source before claiming the production architecture contains them.

## Continuation contract

Resume development directly in `/mnt/data/tpt-work/TPT/powdertoy/`, retain source and test outputs;
**do not package** until asked. Expand tests alongside each new authoritative action. Rerun the
native Xvfb smoke and live WebSocket parity suites after each GUI/multiplayer change. Track the
20 items above separately and never label present upstream UI widgets as complete multiplayer parity.

## Additional verified four-original-GUI smoke

`tests/multiplayer/native_gui_four_player_xvfb.py` launches **four full upstream GUI executable instances**
under Xvfb, joins one server with local physics on all four, and injects a brush gesture through the
visible window. The Linux test **passed**: 4 connected native players, 26 authoritative revisions,
zero canvas video frames, no desync or client termination, and server tick rate ~28.1 Hz in the
sampled busy interval. This is a short local test, *not* an hour-long high-density qualification.

```bash
python3 TPT/powdertoy/tests/multiplayer/native_gui_four_player_xvfb.py \
  ./cmake-build/tpt-original-gui-multiplayer ./cmake-build/tpt-server
```

This supplements, rather than completes, milestone 20.
