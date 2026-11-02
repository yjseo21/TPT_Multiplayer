# Network setup (mp_tls, mp_zlib, ix, mp_webp, mp_codec_args, mp_link_args) is in net/meson.build.
mp_deps = [sta_libs['common'], sta_libs['simulation'], fftw_dep, json_dep, mp_zlib, threads_dep]
if mp_webp.found()
  mp_deps += [mp_webp]
endif
mp_library = static_library('multiplayer-core',
  'Protocol.cpp', 'World.cpp', 'Canvas.cpp', 'Metrics.cpp', 'Autosave.cpp', 'Traffic.cpp', 'Tools.cpp',
  'TickTimeline.cpp', 'CommandRecord.cpp', 'Journal.cpp', 'StateDigest.cpp', 'ExactCheckpoint.cpp',
  'CheckpointWire.cpp', 'ReplicaState.cpp', 'Stroke.cpp', 'FloatingPoint.cpp',
  '../gui/game/tool/Tool.cpp', '../gui/game/tool/ElementTool.cpp', '../gui/game/tool/WallTool.cpp',
  '../simulation/SimTool.cpp', '../simulation/ToolClasses.cpp',
  '../client/GameSave.cpp', '../client/User.cpp', '../graphics/Renderer.cpp',
  '../simulation/Editing.cpp', '../simulation/Snapshot.cpp',
  '../simulation/gravity/Fft.cpp', '../simulation/gravity/DeterministicGravity.cpp', '../gui/game/Brush.cpp', '../prefs/Prefs.cpp',
  include_directories: project_inc, dependencies: mp_deps,
  cpp_args: project_cpp_args + mp_codec_args, override_options: target_options)
mp_core = declare_dependency(link_with: mp_library, dependencies: mp_deps,
  include_directories: project_inc)
mp_server = executable('tpt-server', 'Server.cpp', 'Room.cpp', 'Peer.cpp', 'CanvasProducer.cpp',
  dependencies: [mp_core, ix], cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options, install: true)
mp_replica = executable('tpt-replica', 'Replica.cpp',
  dependencies: [mp_core, ix], cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options, install: true)
mp_client = executable('tpt-remote', 'Client.cpp', 'Session.cpp',
  dependencies: [mp_core, ix, sdl2_dep], cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options, install: true)
mp_tests = executable('multiplayer-tests', '../../tests/multiplayer/CoreTests.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
test('multiplayer-core', mp_tests, timeout: 60)
mp_paste_fuzz = executable('tpt-paste-fuzz', '../../tests/multiplayer/PasteFuzzTests.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
test('multiplayer-paste-fuzz', mp_paste_fuzz, timeout: 300)
mp_element_sweep = executable('tpt-element-sweep', '../../tests/multiplayer/ElementSweepTests.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
test('multiplayer-element-sweep', mp_element_sweep, timeout: 600)
mp_checkpoint_tests = executable('tpt-checkpoint-tests', '../../tests/multiplayer/CheckpointTests.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
test('multiplayer-checkpoint', mp_checkpoint_tests, timeout: 180)
mp_replica_tests = executable('tpt-replica-tests', '../../tests/multiplayer/ReplicaTests.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
test('multiplayer-replica', mp_replica_tests, timeout: 180)


# The timeline test has no simulation dependencies; it also runs standalone
# in developer containers without Meson or the TPT prebuilt dependency pack.
mp_timeline_tests = executable('tpt-timeline-tests',
  'TickTimeline.cpp', '../../tests/multiplayer/timeline_standalone.cpp',
  include_directories: project_inc, override_options: target_options)
test('multiplayer-timeline', mp_timeline_tests, timeout: 10)
mp_wire_tests = executable('tpt-checkpoint-wire-tests',
  'CheckpointWire.cpp', '../../tests/multiplayer/checkpoint_wire_standalone.cpp',
  include_directories: project_inc, override_options: target_options)
test('multiplayer-checkpoint-wire', mp_wire_tests, timeout: 10)
mp_replay = executable('tpt-replay', 'Replay.cpp', dependencies: mp_core,
  cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options, install: true)
executable('tpt-rollback-benchmark', 'RollbackBenchmark.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
executable('tpt-benchmark', 'Benchmark.cpp',
  dependencies: mp_core, cpp_args: project_cpp_args + mp_codec_args, link_args: mp_link_args,
  override_options: target_options)
test('multiplayer-replay', python3_prog,
  args: [files('../../tests/multiplayer/replay_test.py'), mp_server, mp_client, mp_replay], timeout: 180)
test('multiplayer-exact-checkpoint-server', python3_prog,
  args: [files('../../tests/multiplayer/checkpoint_server_test.py'), mp_server], timeout: 90)
test('multiplayer-network', python3_prog,
  args: [files('../../tests/multiplayer/network_test.py'), mp_server, mp_client], timeout: 120)
test('multiplayer-traffic', python3_prog,
  args: [files('../../tests/multiplayer/traffic_test.py'), mp_server, mp_client], timeout: 90)
test('multiplayer-presence', python3_prog,
  args: [files('../../tests/multiplayer/presence_test.py'), mp_server, mp_client], timeout: 60)
test('multiplayer-administration', python3_prog,
  args: [files('../../tests/multiplayer/admin_test.py'), mp_server, mp_client], timeout: 75)
test('multiplayer-tools', python3_prog,
  args: [files('../../tests/multiplayer/tools_test.py'), mp_server, mp_client], timeout: 60)
test('multiplayer-recovery', python3_prog,
  args: [files('../../tests/multiplayer/recovery_test.py'), mp_server, mp_client], timeout: 120)
test('multiplayer-impaired-network', python3_prog,
  args: [files('../../tests/multiplayer/network_impairment_test.py'), mp_server, mp_client],
  suite: 'network-impairment', timeout: 240, is_parallel: false)

# End-to-end authoritative server -> local physics replica with real WebSocket,
# ordered edits, server seals, digest validation and local PNG rendering.
test('multiplayer-local-replica-network', python3_prog,
  args: [files('../../tests/multiplayer/replica_network_test.py'), mp_server, mp_replica],
  timeout: 95, is_parallel: false)

test('multiplayer-local-replica-four-player', python3_prog,
  args: [files('../../tests/multiplayer/replica_four_player_test.py'), mp_server, mp_replica],
  timeout: 95, is_parallel: false)

# Interactive client with local physics: identical canvases and divergence recovery.
test('multiplayer-local-client', python3_prog,
  args: [files('../../tests/multiplayer/local_client_test.py'), mp_server, mp_client],
  timeout: 120, is_parallel: false)

test('multiplayer-room-sweep', python3_prog,
  args: [files('../../tests/multiplayer/room_sweep_test.py'), mp_server, mp_client],
  timeout: 150, is_parallel: false)

test('multiplayer-hostile-input', python3_prog,
  args: [files('../../tests/multiplayer/hostile_input_test.py'), mp_server, mp_client],
  timeout: 120, is_parallel: false)

test('multiplayer-fuzz-room', python3_prog,
  args: [files('../../tests/multiplayer/fuzz_room_test.py'), mp_server, mp_client],
  timeout: 150, is_parallel: false)

test('multiplayer-gravity-room', python3_prog,
  args: [files('../../tests/multiplayer/gravity_room_test.py'), mp_server, mp_client],
  timeout: 90, is_parallel: false)

test('multiplayer-slow-player', python3_prog,
  args: [files('../../tests/multiplayer/slow_player_test.py'), mp_server, mp_client],
  timeout: 90, is_parallel: false)

# Lightweight determinism regression (no engine or network dependencies).
mp_import_seed_tests = executable('tpt-import-seed-tests',
  '../../tests/multiplayer/import_seed_standalone.cpp',
  include_directories: project_inc, override_options: target_options)
test('multiplayer-import-seed', mp_import_seed_tests, timeout: 10)

# The real game in a room, driven through its own controller (headless SDL).
if get_option('build_powder')
  test('multiplayer-game-room', python3_prog,
    args: [files('../../tests/multiplayer/game_room_test.py'), mp_server, mp_client, powder_exe],
    timeout: 90, is_parallel: false)
  # Two real games: A pastes a bunker from its own canvas, B pastes a snow bomb onto it.
  test('multiplayer-bunker-bomb', python3_prog,
    args: [files('../../tests/multiplayer/bunker_bomb_test.py'), mp_server, mp_client, powder_exe],
    timeout: 90, is_parallel: false)
endif
