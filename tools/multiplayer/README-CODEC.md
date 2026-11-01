# Low-bandwidth rendering: build, test, and benchmark

Status: **experimental optional codec implementation, 31 Oct 2026.** The default lossless stream is unchanged. The updated full `tpt-server`/`tpt-remote` executables could **not** be built and integration-tested in this container, because Meson is not installed and external package downloads are unavailable. The compiled `linux-bin/` files in the previous package predate this change. Do not claim those binaries implement WebP. Standalone C++ codec round-trip, corruption, and benchmark checks **have** run successfully on Linux with libwebp 1.5.0.

## How to enable

Install Meson, Ninja, a C++20 compiler, and libwebp headers+library (`libwebp-dev` on Debian/Ubuntu). Then configure/build both server **and** client:

```bash
meson setup build-mp --buildtype=release -Dstatic=prebuilt -Dlua=none \
  -Dhttp=false -Dbuild_multiplayer=true -Dbuild_powder=false -Dresolve_vcs_tag=no
meson compile -C build-mp
meson test -C build-mp --print-errorlogs
```

Meson detects libwebp automatically via `dependency('libwebp', required: false)`. Without it, the build stays lossless; `--webp-quality` nonzero is refused. Check that `meson setup` reports libwebp found. On Linux, runtime libwebp must be present for a binary linked to it; do not distribute the old statically built binary as the new version. On Windows/macOS, arrange a compatible libwebp development package and verify native linking and deployment separately.

Server examples (keep room/admin tokens, TLS, upload budgeting, other options as before):

```bash
# High fidelity: original resolution, video compression artifacts only
./build-mp/src/multiplayer/tpt-server --webp-quality 50 --webp-scale 1

# Small uplink: downscale each dimension by two, then WebP quality 30
./build-mp/src/multiplayer/tpt-server --webp-quality 30 --webp-scale 2 \
  --upload-kbps 2000

# Lowest bandwidth, substantially lower pixel detail
./build-mp/src/multiplayer/tpt-server --webp-quality 30 --webp-scale 3

# Exact original appearance and fallback protocol
./build-mp/src/multiplayer/tpt-server --webp-quality 0
```

Native clients do not need a new CLI flag. They advertise `webp:true` during `hello` only if compiled with libwebp. On `welcome`, `webp`, `webpQuality`, and `webpScale` show the negotiated stream. Legacy clients and automated Python protocol clients do not advertise WebP and continue to receive the original lossless stream, even from a WebP-enabled server. If *any* legacy client connects, it may consume its full share of room upload capacity. TCP/TLS packet overhead is additional.

## Wire format

The existing protocol 3 (`TPC3`, 52-byte header) remains unchanged for modes 0–2: zlib full, zlib XOR delta, and unchanged. New **mode 3** is a self-contained lossy WebP keyframe; it is sent only to clients advertising WebP support. `baseId=0`. Header checksum is CRC32 of the compressed WebP **body** instead of the lossless pixel-plane CRC32. It is checked before WebP parsing/decoding. Width/height must both equal native dimensions divided by the same integer factor 1–3; decoded RGB is nearest-neighbour expanded back to 612×384, then packed as B,G,R planes. Decompressed output allocation is bounded. Do not treat WebP-decoded RGB as the exact server state.

**Critical:** A lossy frame cannot be the baseline for a later XOR delta against the server's original pixels. `CanvasFrame.lossy` is recorded for received lossy frames and after the sender emits a mode-3 packet; `EncodeCanvas` refuses to produce deltas/repeats from such a baseline. WebP frames are independently decodable. Resynchronization accepts modes 0 **or 3** as keyframes. A shared `CanvasPixelsData` stores a single WebP encoding per published snapshot/view, not one encoding per player. One client can consume a stream of different mode-0/1/2/3 packets as long as future deltas reference an actually lossless baseline.

The simulation **never** runs on the remote client in this mode. All tools, collisions, heat, RNG, and physical state remain on the authoritative server. WebP is a presentation-only approximation; screenshots, headless `--output` exports, and heat/pressure views will reflect its loss. For scientific inspection, exact per-pixel colour matters: keep `--webp-quality 0` until a separate exact-state inspect-query API is implemented.

## Reproduce standalone codec tests

This does not require Meson, SDL, or TPT engine linking. Run from the `powdertoy/` root:

```bash
g++ -std=c++20 -O2 -ffunction-sections -fdata-sections -DTPT_MP_WEBP=1 \
  -Isrc src/multiplayer/Canvas.cpp tools/multiplayer/codec_smoke.cpp \
  -Wl,--gc-sections -lz -lwebp -o /tmp/tpt-codec-smoke
/tmp/tpt-codec-smoke

g++ -std=c++20 -O2 -ffunction-sections -fdata-sections -DTPT_MP_WEBP=1 \
  -Isrc src/multiplayer/Canvas.cpp tools/multiplayer/codec_benchmark.cpp \
  -Wl,--gc-sections -lz -lwebp -o /tmp/tpt-codec-benchmark

# Capture real rendered frames from any existing functioning protocol-v3 binaries.
python3 tools/multiplayer/capture_reference_canvases.py \
  /absolute/path/to/tpt-server /absolute/path/to/tpt-remote /tmp/tpt-reference-scenes
/tmp/tpt-codec-benchmark /tmp/tpt-reference-scenes/{water,fire,dust}.raw
```

`capture_reference_canvases.py` imports the existing independent `tests/multiplayer/network_test.py` WS client; it starts an isolated loopback room, adds rectangles of water/fire/dust, unpauses, collects 20 frames per scene, and shuts down. The `.raw` files are **unencrypted original pixels**, not executable or network traffic. They are not needed for release distributions. Benchmark uses a sent-frame baseline identical to the server's existing behaviour, checks reconstructed pixels, and labels lossy baselines. The columns mean an estimated sustained 20 updates/sec under these same conditions, not achieved throughput at real RTT/loss. No WebSocket/TLS overhead is included. PSNR is per-RGB-channel averaged over the entire image and does not indicate legibility of individual particles.

## Results / selection tradeoffs

Measured from the previous protocol-v3 server's actual rendered output, 20 frames per scene, three seeded-but-not-fully-controlled worlds, this Linux container (Oct 31 2026):

| Scene | Lossless Mbps/viewer | WebP Q30 full size | WebP Q30 half width/height | WebP Q30 1/3 width/height |
|---|---:|---:|---:|---:|
| Moving water | 4.538 | 1.448 | 0.155 | 0.058 |
| Fire | 5.774 | 0.215 | 0.094 | 0.065 |
| Dense dust | 3.213 | 1.890 | 0.417 | 0.151 |

The Q30 half-resolution water capture had **21.47 dB PSNR**, compared with **24.22 dB at full resolution**; thin features are lost, not merely blurred. Full-size fire was **42.12 dB**. Encoding per captured view includes zlib+WebP (water Q30 full ~26 ms, half ~13.5 ms on this machine). Four viewers can share the WebP computation for a given view, but independent normal/heat/pressure views cost separate encodes. Real network tests, memory soak, decoder fuzzing, and native Windows builds remain outstanding. Recommended defaults should be calibrated after user visual evaluation; full-resolution WebP is less damaging, half-res is a constrained-network option.

## Unfinished work / robustness

- Run full Meson compilation and all core/network/administration/recovery/traffic/impaired-network tests with the updated binaries, not the old ones.
- Add a first-class full integration test using a native WebP-enabled client and server; it must verify keyframe resync, independent view changes, WebP handshake fallback, room upload limits, reconnect, and four peers.
- Use adversarial WebP frames, wrong dimensions, truncated frames, bad CRCs, and fuzzing under sanitizers.
- Measure actual wired delivery at four players over 100/200 ms RTT, 1–3% loss, 1/2/4 Mbps aggregate uplink. Count TLS/TCP/retransmits on the host.
- Decide whether a proper inter-frame video codec (AV1/H.264/WebRTC) can provide better high-motion fidelity at lower bandwidth, taking latency, native availability and licensing into account. Do not assume WebP keyframes are optimal.
- Implement an exact inspect-query endpoint and lossless focus ROI if reduced-scale frames obscure material state; these are not implemented.
