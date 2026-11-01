project('ixwebsocket', 'cpp', version: '12.1.0', default_options: ['cpp_std=c++20'])
sources = files(
  'ixwebsocket/IXBench.cpp', 'ixwebsocket/IXCancellationRequest.cpp',
  'ixwebsocket/IXConnectionState.cpp', 'ixwebsocket/IXDNSLookup.cpp',
  'ixwebsocket/IXExponentialBackoff.cpp', 'ixwebsocket/IXGetFreePort.cpp',
  'ixwebsocket/IXGzipCodec.cpp', 'ixwebsocket/IXHttp.cpp',
  'ixwebsocket/IXHttpClient.cpp', 'ixwebsocket/IXHttpServer.cpp',
  'ixwebsocket/IXNetSystem.cpp', 'ixwebsocket/IXSelectInterrupt.cpp',
  'ixwebsocket/IXSelectInterruptFactory.cpp', 'ixwebsocket/IXSelectInterruptPipe.cpp',
  'ixwebsocket/IXSelectInterruptEvent.cpp', 'ixwebsocket/IXSetThreadName.cpp',
  'ixwebsocket/IXSocket.cpp', 'ixwebsocket/IXSocketConnect.cpp',
  'ixwebsocket/IXSocketFactory.cpp', 'ixwebsocket/IXSocketServer.cpp',
  'ixwebsocket/IXSocketTLSOptions.cpp', 'ixwebsocket/IXStrCaseCompare.cpp',
  'ixwebsocket/IXUdpSocket.cpp', 'ixwebsocket/IXUrlParser.cpp',
  'ixwebsocket/IXUuid.cpp', 'ixwebsocket/IXUserAgent.cpp',
  'ixwebsocket/IXWebSocket.cpp', 'ixwebsocket/IXWebSocketCloseConstants.cpp',
  'ixwebsocket/IXWebSocketHandshake.cpp', 'ixwebsocket/IXWebSocketHttpHeaders.cpp',
  'ixwebsocket/IXWebSocketPerMessageDeflate.cpp', 'ixwebsocket/IXWebSocketPerMessageDeflateCodec.cpp',
  'ixwebsocket/IXWebSocketPerMessageDeflateOptions.cpp',
  'ixwebsocket/IXWebSocketServer.cpp', 'ixwebsocket/IXWebSocketTransport.cpp',
  'ixwebsocket/IXSocketMbedTLS.cpp',
)
defs = ['-DIXWEBSOCKET_USE_TLS', '-DIXWEBSOCKET_USE_MBED_TLS', '-DIXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3']
deps = [dependency('threads'), dependency('tpt-mp-tls')]
if host_machine.system() == 'windows'
  cc = meson.get_compiler('cpp')
  deps += [cc.find_library('ws2_32'), cc.find_library('crypt32')]
endif
lib = static_library('ixwebsocket', sources, cpp_args: defs, dependencies: deps)
ixwebsocket_dep = declare_dependency(link_with: lib, include_directories: include_directories('.'),
  compile_args: defs, dependencies: deps)
