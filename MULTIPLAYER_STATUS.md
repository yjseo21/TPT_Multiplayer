# Newest verified milestone: command-only local physics (2026-10-31)

**Runtime-tested on Linux in this iteration.** Exact checkpoints, ordered WebSocket commands, tick seals and client-side simulation/rendering now work in a noninteractive local replica. Three independent replicas and one editor passed a four-participant room test, and a 10,000-tick independent physics replay matched exactly. This supersedes any older status below that says exact checkpoint runtime tests or local simulation were not implemented.

Read **`docs/multiplayer/LOCAL_PHYSICS_REPLICA_2026-10-31.md`** for tested commands, exact payload protocol, bandwidth numbers, hazards, implementation file map and remaining work. GUI, automatic desync recovery and cross-platform determinism are not complete.

---

## Experimental exact checkpoint continuation (2026-10-31)

Source now includes `ExactCheckpointV1` and optional server `--exact-load`/`--exact-save`. Read `docs/multiplayer/EXACT_CHECKPOINT_V1_2026-10-31.md`. The new runtime code is **not yet fully compiled or executed** in this container. It is not client-side lockstep, not a replacement for the image stream, and not a qualified backup. Existing results below refer to earlier server builds.

**Prototype status — 30 October 2026**

The dedicated server runs The Powder Toy physics and renderer, receives editing commands from up to four clients, and streams a lossless shared canvas to native SDL clients. The bandwidth milestone replaced complete particle snapshots with canvas deltas. Protocol v3 adds capture timestamps and connection telemetry. Normal, heat and pressure views remain independent per player.

Built under Ubuntu/WSL with GCC 13.3.0 in release mode. Both the original desktop application and the new multiplayer executables compile. Native Windows builds are not yet verified.

The core and integration tests pass. They cover authoritative physics stepping, editing while paused, exact state/canvas round trips, upstream pixel agreement in all three views, malformed inputs, missing baseline detection, byte budgeting, four-client agreement, independent views, keyframe resync, room capacity, protocol/authentication checks, permissions, duplicate requests, late join, native display, standard save/load, oversized WebSocket frames, trusted/untrusted TLS, and graceful shutdown. A busy-liquid network test verifies that a stalled receiver stops at one initial unacknowledged frame, another player continues receiving while physics advances, and resumed delivery reconstructs correctly. Clearing a scene also clears accumulated glow.

The PowerShell build and launch helpers were verified on 30 October. A local demo used two editing clients and a third native renderer, which received 30 frames through the live server. Its output is `build-mp/demo/shared-canvas.png`, and the saved scene is `build-mp/demo/shared-world.cps`. Reproduce it with `tools/multiplayer/make-demo.py` using the server path, client path and output directory as arguments.

**Bandwidth measurements after optimization**

Run `build-mp/src/multiplayer/tpt-benchmark` to compare both codecs against the same scenes. The benchmark runs 120 physics steps and 40 captures per scene with simulation RNG seed 12345. Renderer flicker retains upstream randomness. Results below are from the protocol-v2 release build on this development machine; the CSV is `build-mp/bandwidth-benchmark.csv`. Protocol v3 adds eight timestamp bytes per canvas packet and a small telemetry exchange each second; the pixel codec is unchanged. These short synthetic runs are not internet playtests, sustained load tests, or hardware-independent performance guarantees.

| Scene | Initial particles | Full state at 20 Hz | Canvas at 20 Hz | Reduction | Physics step | Shared render/pack/compress | Per-client delta encode |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Static metal | 6,000 | 9.00 Mbps | 0.02 Mbps | >99% | 0.74 ms | 0.38 ms | <0.01 ms |
| Moving water | 30,000 | 120.35 Mbps | 6.17 Mbps | 94.9% | 3.53 ms | 5.36 ms | 4.96 ms |
| Dense dust | 180,000 | 299.20 Mbps | 7.11 Mbps | 97.6% | 11.06 ms | 7.51 ms | 5.58 ms |
| Fire | 15,000 | 33.13 Mbps | 4.55 Mbps | 86.3% | 0.92 ms | 2.45 ms | 1.57 ms |

Bandwidth estimates include the initial keyframe amortized across 40 captures and exclude WebSocket/TLS overhead. The static-metal benchmark sends repeated metadata at 20 Hz to compare codecs; the actual server suppresses paused captures and uses a 52-byte heartbeat once per second in protocol v3. Without a shared cap, four clients approximately multiply server egress by four; the new room budget limits this by reducing update rates. Rendering/full-frame compression is shared per requested display mode. Snapshot copying remains on the physics thread; rendering and delta encoding run on workers. The benchmark assumes 20 updates/sec; CPU, RTT and bandwidth limits can reduce delivered rates.

The current default canvas budget is 8.192 Mbps for the whole room (`--upload-kbps 8192`), with an additional 8.192 Mbps per-player ceiling (`--bandwidth-kbps 8192`). The room has one shared burst of about 709 KB; bounded control traffic is additional. The measurements below predate the shared budget. Lower budgets reduce update frequency while preserving pixel fidelity and server physics. In the protocol-v2 128 kbps integration test, the fast reader received 769,996 payload bytes over 6.34 seconds, within the one-keyframe burst plus sustained 16,000-byte/sec envelope. The deliberately slow reader remained bounded and resumed successfully. The same budget/flow-control test is retained for protocol v3. This short test verifies the budget and flow control, not long-session network quality.

The canvas is original-resolution lossless RGB. Physics-only fields stay on the server; future particle inspection requires a query. Full keyframes are selected whenever they compress better than deltas, on join, after a display-mode change, or for explicit resynchronization. Both executables must be upgraded together to protocol 3 / `tpt-c205becb-mp3`.

**Connection visibility and impaired-network milestone**

The native client now has a status panel below the canvas (F4 toggles it). It shows application ping, received updates/sec, application payload Mbps, achieved server physics steps/sec, mean physics-step cost, estimated canvas age, the configured bandwidth budget, and outstanding frame acknowledgements. It distinguishes stale telemetry, bandwidth throttling, slow simulation, delayed updates, high latency and a paused world. Capture timestamps include server rendering/encoding/queue delay in the age estimate. Clock offset comes from a four-timestamp probe; asymmetric paths make age approximate. Paused canvases are explicitly labelled instead of reporting an intentional freeze as a stall.

`--metrics PATH` writes JSONL samples, `--run-for SECONDS` supports bounded diagnostic sessions, and `--output-hud PATH` exports the canvas with its status panel. The ordinary `--output` export remains canvas-only. The core tests exercise independent clocks, server queue-time exclusion, capture age, byte/update rates, expired telemetry and slowdown/throttle classification.

The automated `multiplayer-impaired-network` test uses Linux `tc netem` in a disposable unprivileged user/network namespace. Host network settings are untouched. Each scenario has two native clients and two independently implemented protocol clients, concurrent edits, physics, a heat-view switch, a return to normal view, and final paused-canvas checksum/pixel agreement. Actual kernel drops are checked; this is not application-byte deletion or a loss-free delay proxy. One native client exercises SDL's dummy display.

Recorded four-player run (`build-mp/impairment/report.json`):

| Nominal base RTT | Jitter per leg | Packet loss | Shared link rate | Observed drops | Median application ping under load | Median estimated canvas age | Median updates/sec |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50 ms | 3 ms | 1% | 8 Mbps | 13 | 361.5 ms | 247.0 ms | 10.5 |
| 100 ms | 5 ms | 1% | 4 Mbps | 12 | 355.1 ms | 410.2 ms | 8.0 |
| 200 ms | 10 ms | 3% | 2 Mbps | 40 | 828.7 ms | 772.6 ms | 4.0 |

All three passed and all four clients converged. The 200 ms case temporarily reduced the link to 512 kbps for two seconds, then recovered. Simulation remained near 60 steps/sec during sustained running. These rates constrain all four players' traffic on the namespace's loopback, so they are shared capacities, not per-player budgets. Reported application ping can greatly exceed nominal RTT because TCP loss recovery and queued canvas traffic delay the probe. Results are short-run samples, vary with random loss and physics, and do not establish long-session memory stability or production readiness. They identify congestion-aware pacing and a shared server upload budget as further improvements.

The test is included in normal Meson checks and `build-wsl.ps1 -Test`; unavailable Linux tooling/user namespaces produce an explicit skip, not a pass. See the README for the command that retains logs, metrics and screenshots. The inspected screenshot is `build-mp/impairment/rtt-50/hud-0.png`.

**Next work**

Automatic reconnect, autosave/recovery, shared upload budgeting/adaptive pacing, and player presence/shared cursors are implemented (details below). Next are sustained real-internet sessions and deployment qualification, followed by the broader tool UI. Exact versioned transient-state checkpoints remain outstanding. The configurable budget bounds traffic but does not guarantee 20 Hz delivery for every world. Native Windows builds remain unverified.

**Reconnect and save-recovery milestone**

The client retries transient failures with 1/2/4/8/10-second backoff, including connecting before the server starts. A frozen last canvas and reconnect status remain visible. Editing is gated until the new session supplies a complete canvas in the selected view. Material, radius, name and view remain in memory. Each connection starts with fresh frame baselines, editing sequences and telemetry state. Unconfirmed edits are counted and discarded, never replayed. Invalid credentials, incompatible protocol and certificate-verification failures are terminal; `--no-reconnect` provides fail-fast behavior. Room-full replies use a retryable close code. The wire format remains protocol v3.

`--autosave-dir DIRECTORY` enables periodic standard TPT backups (60 seconds and five retained generations by default). `--recover` reads the newest valid committed generation, falling back over corrupt files and ignoring incomplete temporary files. No valid committed save means startup fails instead of silently replacing it with an empty world. Empty directories can start a new world. One server holds an exclusive directory lock. Capture runs on the simulation thread; a bounded worker serializes, validates, flushes and atomically commits each save before pruning older generations. Unchanged paused worlds skip periodic writes. Graceful shutdown commits a final save, and ordinary `--save` exports now use atomic replacement too. Failures preserve committed backups and periodic writes retry after storage recovers.

The reconnect milestone release build passed all four then-existing Meson suites: core (0.22 s), network/TLS (18.42 s), recovery (35.95 s) and impaired network (84.88 s). The recovery suite runs one native SDL client through initial connection refusal, a live TCP disconnect while the server keeps running, and two forced server terminations. It verifies four successful connections, heat-view restoration, fresh sequence/baseline handling and final canvas agreement. It also checks retention, an exclusive lock, a read-only-directory failure followed by successful retry, corrupt-newest fallback, ignored partial files, all-corrupt startup rejection, permanent credential rejection and fail-fast mode. The independent edit-session unit test verifies that ambiguous operations are dropped rather than replayed.

A separate bounded server run verified atomic save/recovery in `build-mp/recovery-drive` on the Windows-mounted workspace under WSL. The recovery integration test uses native Linux temporary storage for meaningful permission-failure testing and is explicitly skipped for root/non-Linux runs. Logs and a screenshot from an earlier retained recovery run are in `build-mp/recovery`; the latest suite results are in `build-mp/meson-logs/testlog.txt`.

**Shared upload budget and adaptive pacing milestone**

Final release validation passed all five Meson suites under Ubuntu WSL: core (0.21 s), traffic (12.83 s), network/TLS (18.03 s), recovery (35.45 s), and impaired network (84.03 s), with no failures or skips. See `build-mp/meson-logs/testlog.txt`.

The server now defaults to a shared 8.192 Mbps canvas cap (`--upload-kbps 8192`). Connected players get equal shares, each additionally bounded by `--bandwidth-kbps`. Idle or ceiling-limited shares are not borrowed. A single shared 709,120-byte token-bucket burst survives membership and view changes. FIFO eligible requests reserve room credit for larger keyframes; abandoned reservations expire after 250 ms. A stalled connection waiting for ACKs does not reserve shared credit.

Each stream starts with one outstanding frame and allows at most two after its first ACK. The send interval grows immediately with ACK delay and recovers gradually, while overdue ACKs prevent additional backlog. Pending captures still coalesce without breaking delta baselines. The client HUD shows the current share, pacing ceiling and ACK window; JSONL also exposes room/per-player caps, smoothed ACK delay and pacing status. This is a bounded application-level pacing heuristic, not a link-capacity estimator or replacement for TCP congestion control.

The four-player traffic integration test delivered 1,377,369 canvas payload bytes over 11.93 seconds under a shared 512 kbps cap, inside the single-burst plus sustained-rate envelope. It verified equal allocations, continuing delivery to every player, allocation changes after departure, and identical final paused canvases. Unit tests cover shared burst accounting, reconnect resistance, keyframe starvation prevention, reservation expiry, rate changes and pacing slowdown/recovery. The existing stalled-reader test now verifies the initial one-frame ACK gate. The impairment suite checks the adaptive two-frame maximum and pacing telemetry under real kernel latency/loss. This remains short-run qualification; sustained internet sessions and native Windows builds are outstanding. Large keyframes on extremely low budgets can still encounter synchronization or ACK timeouts.

**Player presence and shared cursor milestone**

All six release-build suites passed under Ubuntu WSL: core (0.24 s), presence (7.12 s), traffic (12.93 s), network/TLS (18.51 s), recovery (35.38 s), and impaired network (83.74 s), with no failures or skips. The retained SDL HUD image was also visually checked for the remote cursor/name and player roster.

Clients opt in to presence through the existing version-3 hello/welcome exchange. The server sends a bounded roster of connected IDs, sanitized display names and admin roles, plus cosmetic brush position/radius/drawing state. IDs come from the connection, never client cursor fields. Names are not unique identities. Legacy clients continue to work without unsolicited presence messages.

The native panel lists up to four players; remote outlines and name labels are drawn locally over the authoritative canvas. Focus loss/window exit hides the local shared cursor, stale cursor positions expire after two seconds, and disconnect clears the client roster before reconnect. Cursor messages do not consume command sequences or mutate the world. The client sends at most 10 updates/second with one-second keepalives; the server caps accepted input at 20/second with a burst of 20, broadcasts at most 10 snapshots/second, and replaces unsent snapshots. Presence adds bounded control traffic outside the canvas budget. HUD exports include presence; raw canvas exports and checksums exclude it.

The new presence integration suite checks join/leave/rejoin, role/name display, control-character sanitization, connection-owned cursor identity, coordinate bounds, stale expiry, cursor flood isolation, unchanged editing sequences and a real SDL client roster. Unit tests reject duplicate IDs and incomplete visible cursors. Retained native metrics and a HUD screenshot are in `build-mp/presence`. Initial room moderation is implemented below; sustained internet qualification remains future work.

**Room administration milestone — Linux container continuation**

Implemented server-enforced join locking and player removal without changing the version-3 canvas protocol. Existing connections remain when locked; admins can join a locked room. Only admins may send sequenced `op: admin` lock/kick commands. Denied actions consume their sequence; admins cannot kick themselves or each other. Kicked clients receive a policy close and editing requests still queued after their disconnect are ignored. Moderation is rate- and queue-limited and handled by the authoritative server thread without advancing physics tick/revision.

The native client uses Ctrl+L to lock/unlock and Ctrl+Shift+1–4 to kick a corresponding non-admin roster occupant. Admin commands are supported by the native JSONL script runner. The lock appears in the window title and metrics and resets when the server restarts. Kick is not permanent banning or revoking the room credential. New `multiplayer-administration` tests exercise permissions, sequence consumption, join denial, administrator bypass, protected targets, disconnect reasons, preserved physics state and native scripted commands.

The Linux server and client were rebuilt using the archive's cached Meson build commands and static dependency pack on Debian. In this container, administration, core, tools, presence, network/TLS and shared-traffic tests passed. Recovery's POSIX permission-failure test requires a non-root account, and impaired-network qualification requires network namespace capabilities; these are not new verified passes in this container.

These backups provide standard-save crash recovery, not exact continuation of every runtime field. Crashes can lose edits since the last committed save; tick/revision counters and connection identities restart. Long-session testing and a native Windows build remain outstanding. See the README for the persistent-server launch command and diagnostics.

See [README.multiplayer.md](README.multiplayer.md) for building, launching, controls, credentials, protocol details and deployment constraints.


---

**31 October 2026 continuation — source changes, not a new qualified release**

Added opt-in WebP still-image streaming with full/half/third image resolution, capability negotiation, compressed-payload checksum, lossless legacy fallback, and a lossy-baseline marker to prevent invalid XOR deltas. The native client's resync logic accepts WebP keyframes. Codecs are benchmarked against 20 real rendered frames each of water, fire and dust (see `tools/multiplayer/README-CODEC.md`). The half-size WebP Q30 test estimates 0.155 Mbps water, 0.094 Mbps fire and 0.417 Mbps dense dust per viewer at 20Hz versus 4.538/5.774/3.213 Mbps losslessly. These are short software codec estimates and intentionally reduce visual fidelity. Codec standalone compile/smoke and fallback tests pass in Linux. Full updated Meson/game/server/native-client build and network integration tests **remain unverified** because this container lacks Meson and cannot download it. Do not distribute old `linux-bin` executables as newly optimized; do not claim every feature has been implemented.


## 31 October 2026 — deterministic replay research milestone

- Added: `TickTimeline`, validated `CommandRecord`, opt-in server `command_commit`, fixed-seed `--journal` JSONL writer, `StateDigest`, `tpt-replay` command replay verifier, standalone C++ tests, Meson-integrated integration tests.
- Passed in container: timeline and command-payload standalone tests; C++ syntax checks of new implementation; Python test syntax.
- Not run: full Meson build (Meson unavailable), end-to-end WebSocket replay integration, Windows/macOS builds, exact checkpoint restore, real client-side physics, 10k-tick determinism and WAN bandwidth testing.
- Fixed inherited source compilation bug in WebP CoreTests comparison (`shared_ptr` vs string).
- Detailed continuation: `docs/multiplayer/LOCKSTEP_SPIKE_2026-10-31.md`.
