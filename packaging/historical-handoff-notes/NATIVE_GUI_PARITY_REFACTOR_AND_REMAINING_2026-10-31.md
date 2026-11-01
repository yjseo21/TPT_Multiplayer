# Original TPT GUI / deterministic multiplayer — continuation handoff

Date: 2026-10-31. Source root: `/mnt/data/tpt-work/TPT/powdertoy/`. This report describes **implemented, compiled and tested changes**, not an assertion of complete upstream parity.

## Goal and invariants

Use the **original upstream The Powder Toy GUI** as the player-facing editor, with two separate tabs. Singleplayer physics and edits are private; Multiplayer adopts the server's exact checkpoint and ordered edit stream, renders locally, and never sends routine video frames. A user can draw in Singleplayer, Ctrl+C, switch tabs, and Ctrl+V to paste the selected OPS region into Multiplayer as one authoritative edit. Source and tool UI components must remain upstream where possible; never apply mutating commands solely to the multiplayer client's simulation.

**Particle-grid capacity remains 612 × 384 = 235,008**, at the user's request; no million-particle engine change was made.

## Source changes in this continuation

- `src/multiplayer/Protocol.h` and `.cpp`: new server-ordered `replace_save`, `history_mark`, `undo`, `redo`, and compressed `decoration_mask` commands. Larger asymmetric stock brush radii (up to half the canvas in each axis) and edge coordinates. Optional optimistic sign identity checks. Command work budget accounts for **decoded fill pixel count** instead of compressed byte length. Current identical-build handshake identifier: `tpt-native-gui-parity-20261031-v8`; all companion binaries and test fixtures must match.
- `src/multiplayer/DecorationMask.h` (new): flood selection uses the **initiating client's original RendererFrame**, RGB Manhattan difference less than 15, four-connected flood. Packs 235,008 selection pixels into 29,376 bytes, compresses using zlib level 3, prepends `DFM1` format signature, then base64 encodes the payload for the ordered command. Server enforces fixed expanded size and decompression success. Each peer applies the same packed selection via the upstream `Simulation::ApplyDecoration(..., DECO_DRAW)`. It does not depend on the other players' render palettes.
- `src/multiplayer/World.cpp` and `.h`: transactional, administrator-restricted whole-world load using original OPS save data and the actual Simulation factory. Preserve save settings, pause, RNG, and selected world fields; reject Newtonian-gravity saves not supported by exact recovery. Shared history uses compressed exact checkpoints, max **16 entries / 32 MiB** across undo and redo, enabled **only** on server and journal replay. Undo/redo preserve the authoritative tick/revision progression and increment `resetGeneration` for complete client rebase. Server must never broadcast an undo as a normal replay edit.
- `src/multiplayer/Server.cpp`: authoritative history resync triggers fresh checkpoint for every local replica; admission checks protect in-flight transfers and admin-only operations. Canvas/video observers continue as legacy fallback.
- `src/multiplayer/ReplicaState.cpp`: `replica_reset` forces a checkpoint-based recovery; test includes independent WebSocket replica.
- `src/multiplayer/NativeSession.cpp` and `.h`: unified OPS serialization helper shared by original stamp paste and full save replacement; selection history enables Reload Sim for the last save source chosen in Multiplayer. Native room chat and capped cursor telemetry, roster and recent messages, remote cursor positions, marshaled to GUI thread only. Sends render-selected decoration fill without changing the client's authoritative particle data. New original sign edits include old sign text/coordinates/justification to refuse conflicting changes.
- `src/gui/game/GameController.cpp`, `GameView.cpp`, and `src/gui/game/tool/SignTool.cpp`: retain native upstream UI while wiring multiplayer commands. Original decorations (including flood), property editing, save load/export, copy/paste, shared admin undo/redo, chat, cursor overlays, and toolbar/keyboard actions operate through the authoritative server as described. A selected local/online save can be reloaded from the multiplayer tab; absence of selected save reports an error, not a local mutation.
- `src/multiplayer/CommandRecord.cpp`: supports every new server-ordered command for replay/journaling, including sign expectations and renderer-derived fill masks.
- `tests/multiplayer/TabPasteTests.cpp`: authority, malformed input, exact 120-tick world-load replay, brush boundaries, renderer flood selection, sign conflict, history restoration, and private-tab isolation.
- `tests/multiplayer/native_gui_parity_network_test.py`: live WebSocket operations include compressed decoration masks and stale sign detection.
- `tests/multiplayer/native_history_journal_test.py`, `native_history_network_test.py`: replay/recovery and admin permission checks over real WebSockets.
- `tools/multiplayer/run_native_parity_checks.sh` (new): centralized native GUI regression suite; individual tests have 90-second Linux timeouts. Set `TPT_SKIP_XVFB=1` to execute the headless core only.

## Rebuild and test (Linux)

The standalone development harness is at `/mnt/data/tpt-work/CMakeLists.txt` (one directory **above** source root). It exists because the container's standard Meson setup was not available; it is **not** a verified replacement for the supported upstream Meson build.

```bash
cmake --build /mnt/data/tpt-work/cmake-build \
  --target tpt-server tpt-replica tpt-replay tpt-original-gui-multiplayer \
           tpt-checkpoint-inspect tpt-paste-tests settings_core replica_core checkpoint_core -j4

/mnt/data/tpt-work/TPT/powdertoy/tools/multiplayer/run_native_parity_checks.sh \
  /mnt/data/tpt-work/cmake-build
```

Live GUI check requires `Xvfb` and Python `Xlib`; use an available graphical desktop for normal GUI usage. Check the logs of **both** server and client before claiming parity. All builds must use the same BuildId and physics ABI. An error `checkpoint engine build mismatch` commonly means a test helper executable is stale; rebuild `tpt-checkpoint-inspect`, not just the server.

Start a paused test room:

```bash
export TPT_ROOM_TOKEN='set-a-unique-room-secret'
export TPT_ADMIN_TOKEN='set-a-distinct-admin-secret'
/mnt/data/tpt-work/cmake-build/tpt-server --paused --tick-rate 30
```

In a graphical session, start `/mnt/data/tpt-work/cmake-build/tpt-original-gui-multiplayer disable-network`, click Multiplayer and use the native connection dialog. Remote connections require **`wss://`** and a trusted TLS certificate; `ws://` is restricted to loopback. The original TPT save browsing service is separately controlled by upstream's `disable-network` argument.

## Verification completed in this iteration

- GCC/Linux full original GUI, server, replica, replay, checkpoint inspector and regression binaries **compiled**.
- `settings_core`, `checkpoint_core`, `replica_core`, `tpt-paste-tests`: **passed**. Includes original brush geometry, save replacement, fill-mask parsing and deterministic replay, stale sign conflict, shared history, 120-tick exact restoration.
- `native_gui_parity_network_test.py`: **passed**, 15 accepted original GUI wire actions on two independent live WebSocket viewers; a stale sign delete and unauthorised server edit were rejected, pixel content matched.
- `native_history_journal_test.py`: **passed**, replayed six server-ordered edits with two state checks.
- `native_history_network_test.py`: **passed**, independent local replica matched post-history exact checkpoints and protected admin-only history.
- `native_gui_connect_dialog_xvfb.py`: **passed**, connection via original GUI modal using masked token.
- `native_gui_four_player_xvfb.py`: **passed**, four original TPT windows sharing authoritative physics, with zero server video frames in the smoke test.
- `native_gui_tab_clipboard_xvfb.py`: **passed**, 204 particles copied from private Singleplayer and pasted via Multiplayer, verified from authoritative exact checkpoint (`revision=1`).
- The headless acceptance runner with `TPT_SKIP_XVFB=1` completed and printed `PASS: all requested Linux native GUI regression checks completed`; captured in `/mnt/data/tpt-work/native-gui-headless-acceptance-2026-10-31.log`. Full Xvfb acceptance checks were also run individually.

The network suite has occasionally taken longer than expected when grouped; it is timeout-guarded. A short localhost/Xvfb smoke is not a sustained packet-loss test or proof of cross-platform physics determinism.

## Exact 20-feature-group progress and exclusions

| Milestone group | Verified/current status |
| --- | --- |
| 1. Original desktop window, menus | Upstream source retained; Xvfb launched |
| 2. Local physics networking / original app | Tested, four original GUI windows, zero video frames |
| 3. Element selection | Existing upstream menus retained; exhaustive selection QA open |
| 4. Brush shapes and sizes | Ellipse/rect/triangle asymmetric radii and edges tested; extremely expensive strokes deliberately bounded |
| 5. Point/line/rectangle/fill | Core command replication tested; complete combination matrix open |
| 6. Elements, walls, simulation tools | Selected tests pass; exhaustive element parity open |
| 7. Decoration/color blending | Point/line/box and renderer-selected fill tested; all modes/UI QA open |
| 8. Property editor | Basic property commands pass; all property combinations open |
| 9. Signs and buttons | Add/update/delete/move/button protocol tested, conflict guard added; visual workflow QA open |
| 10. Options dialogs | Original dialogs retained; supported server settings wired; full UI QA open |
| 11. Shortcuts/reset | Paused edits/step/reset settings tested, private vs multiplayer branches present |
| 12. Eyedropper/inspector | Partially present; original behavior QA open |
| 13. Copy and cut | Copy private → shared verified; complete transform/cut cases open |
| 14. Stamps/paste | Cross-tab OPS placement tested; >512KiB OPS transfers not supported yet |
| 15. Multiplayer undo/redo | Exact server checkpoints and automatic rebase verified; **admin-only** and bounded, not singleplayer-identical |
| 16. Save/load and online browse | Local + online authoritative save replacement implemented, private export, reload from last selected save; untested full browser/login/upload parity |
| 17. Lua and mods | **Not complete**. Mutating Lua is intentionally blocked in multiplayer to prevent silent divergence; local SP remains upstream |
| 18. Renderer/HUD/zoom | Upstream rendering retained, cursors drawn locally; exhaustive native display QA open |
| 19. Chat/roster/cursors | Native UI/telemetry implemented and compiled; full visual and internet QA open |
| 20. Final release platform/performance QA | **Not complete**. No native Windows/macOS, 60-minute dense-world or official Meson release qualification |

**Important unresolved technical risks:** shared history encodes exact checkpoints on the authoritative physics thread and can introduce stalls for dense worlds; support for arbitrary Lua/mod-created elements requires deterministic state export and exact checkpoint coverage; large saves/stamps need chunked authenticated commands and bounded admission; collaborative sign guards are optional for older protocol senders; native UI action acknowledgments do not yet provide per-action progress/rollback; server save load requires admin permission; high-density scenes may under-run real-time 30 Hz; full per-element QA is infeasible without a larger automated matrix. Windows/macOS differences remain unqualified. All these limitations must remain explicit in release notes.

## Suggested next engineering sequence

1. Refactor mutating GUI tools behind one tested `MultiplayerActionBridge` with explicit operation results and stale-state/permission errors. Keep one private editor copy and one authoritative replica; never reuse GUI pointers after checkpoint rebase.
2. Move shared history snapshot compression off the physics thread, preserve ordered mark boundaries, add latency instrumentation and dense-load tests.
3. Add chunked authenticated OPS transfer for large stamps and saves, with cancellation, backpressure and checkpoint-safe atomic commit.
4. Build reproducibility tests for broader elements, walls, fields, Lua-triggered elements, Windows/macOS compilers, and multiple CPU architectures. Do not allow unverified Lua mutations just to make the UI look complete.
5. Verify native sign/chat/cursor dialogs with input injection, add save browser and property/decoration tool end-to-end screenshot and physics assertions, and perform 60-minute traffic-controlled multi-client soak.
6. Build and package using the upstream-supported Meson toolchain, test TLS/internet packet loss, and document official user-facing controls and changes to upstream compatibility.

No archive was created in this continuation; source remains directly editable in the Linux working tree.
