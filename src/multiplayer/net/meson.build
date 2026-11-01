# Shared multiplayer setup, processed before the game executable is defined:
# network dependencies, and the library that puts multiplayer into the game.
if host_platform not in ['linux', 'windows', 'darwin']
  error('Multiplayer supports Windows, Linux and macOS')
endif
# Windows reserves 1 MiB of stack by default (Linux: 8 MiB), which simulation
# objects on the stack overflow. std::thread inherits the executable's default.
mp_link_args = project_link_args
if host_platform == 'windows'
  mp_link_args += cpp_compiler.get_linker_id().startswith('link') ? ['/STACK:16777216'] : ['-Wl,--stack,16777216']
endif
if static_variant == 'prebuilt'
  # The upstream dependency pack includes Mbed TLS 3 and zlib with its curl build.
  mp_tls = dependency('libcurl', static: true)
  mp_zlib = tpt_libs.get_variable('zlib_dep')
else
  mp_tls = declare_dependency(dependencies: [
    cpp_compiler.find_library('mbedtls'), cpp_compiler.find_library('mbedx509'),
    cpp_compiler.find_library('mbedcrypto'),
  ])
  mp_zlib = dependency('zlib')
endif
meson.override_dependency('tpt-mp-tls', mp_tls)
# MSVC: the subproject must use the same C runtime as everything else
# (static with the static dependency pack), or the standard library links twice.
ix_options = []
foreach option : target_options
  if option.startswith('b_vscrt=')
    ix_options += option
  endif
endforeach
ix = subproject('ixwebsocket', default_options: ix_options).get_variable('ixwebsocket_dep')
# Optional image encoder/decoder. Builds without libwebp remain strictly lossless.
mp_webp = dependency('libwebp', required: false)
mp_codec_args = []
if mp_webp.found()
  mp_codec_args += ['-DTPT_MP_WEBP=1']
endif

# Multiplayer sources only: the game itself provides the engine and interface.
mp_game_library = static_library('multiplayer-game',
  '../Protocol.cpp', '../World.cpp', '../Canvas.cpp', '../Metrics.cpp', '../Autosave.cpp', '../Tools.cpp',
  '../CommandRecord.cpp', '../StateDigest.cpp', '../ExactCheckpoint.cpp', '../CheckpointWire.cpp',
  '../ReplicaState.cpp', '../Session.cpp', '../GameSession.cpp', '../Stroke.cpp', '../FloatingPoint.cpp',
  include_directories: project_inc,
  dependencies: [ix, mp_zlib, json_dep, threads_dep, mp_webp, sta_libs['common'], sta_libs['simulation']],
  cpp_args: project_cpp_args + mp_codec_args, override_options: target_options)
mp_game_dep = declare_dependency(link_with: mp_game_library, dependencies: [ix, mp_zlib, mp_webp],
  link_args: host_platform == 'windows' ? mp_link_args : [])
