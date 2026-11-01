# TPT Multiplayer — recovered source and new continuation, 31 October 2026

## What this archive actually contains

This archive is a **source-only development snapshot**, not a production release.
It contains the complete recoverable 30 Oct 2026 TPT local-physics prototype source
(`TPT-local-physics-source-2026-10-31.zip`) plus the code and tests added during
this continuation. The original upstream TPT license and resources are included.

### Newly implemented in this continuation

- `src/multiplayer/ImportSeed.h`: deterministic 32-bit seed from FNV-1a hash
  of full, identical imported save-file bytes, used by `World::Load`. If the
  save embeds an RNG state, the embedded state still takes precedence.
- `src/multiplayer/ExactCheckpoint.h/.cpp`: split raw owner-thread exact state
  capture from immutable zlib compression, retaining the existing packet,
  CRC-32 and validation format. Compression **can** be sent to a worker, but
  this recovered source does **not** implement the later asynchronous
  `HistoryCompressor`/GUI history integration.
- `tests/multiplayer/import_seed_standalone.cpp`: deterministic seed regression.
- `tests/multiplayer/CheckpointTests.cpp`: checks raw->compression identity and
  rejection of empty raw state (requires full engine build to run).
- Multiplayer BuildId changed to `tpt-c205becb-mp3-seedfix1` to avoid
  permitting mixed physics versions in a replica room.

### Historical work not present in this recovered source

Later development notes from 31 Oct documented an original TPT GUI with
Singleplayer/Multiplayer tabs and 204-particle cross-tab paste, private-state
handling, admin history, `HistoryCompressor`, online-save UI, remote chat,
shared cursors and additional parity tests. **The corresponding source files
were not available for this packaging session.** They are *not* part of this
snapshot. Files under `packaging/historical-handoff-notes` are a reference
for recovering/reimplementing that newer work, not claims about this build.

### Build (Linux, from `powdertoy/`)

Requirements: Python 3, Meson, Ninja, C++20 compiler, dependencies detailed
in `README.md` and `README.multiplayer.md`. The repository contains a Linux
prebuilt dependency subproject for the supported target.

```bash
python3 -m pip install meson ninja
python3 tools/multiplayer/build.py --build-dir build-mp --jobs 6 --test
# Expected targets include build-mp/src/multiplayer/tpt-server,
# tpt-remote, tpt-replica, tpt-replay, tpt-benchmark.
```

The server, original prototype remote renderer and local-physics replica
are NOT the later integrated original-GUI multiplayer executable. Check
`README.multiplayer.md` for server flags, authentication and local/TLS rules.

### Validation scope

Performed in packaging environment with GCC, source only:

- standalone `import_seed_standalone.cpp`: PASS
- standalone `TickTimeline` suite: PASS
- standalone `CheckpointWire` suite: PASS
- ZIP integrity: checked before distribution

The full Meson build, original GUI and newer network integrations were **not**
run on the recovered snapshot; Meson was unavailable here. The Linux binaries
from the earlier version must not be combined with these changed sources because
of the physics BuildId bump. Cross-platform determinism and 60-minute/WAN
qualification remain open. Do not assume full original TPT GUI parity.

## Provenance and patch

The source originated from `TPT-local-physics-source-2026-10-31.zip` in the
user's file library. `packaging/continuation.patch` is a unified diff of all
modified/new source and tests against that exact archive. `packaging/manifest.json`
contains SHA-256 checksums of all files in this source package other than the
manifest itself. Historical notes are included verbatim for traceability.
