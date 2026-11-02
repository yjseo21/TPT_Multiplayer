# TPT multiplayer: Newtonian gravity in rooms

**Engineering handoff, 2 November 2026.**

## Why the game's solver can't be used

TPT's Newtonian gravity (`src/simulation/gravity/Fft.cpp`) convolves the mass
map with a 1/r² kernel using FFTW, on a worker thread. Two things make its
forces differ between machines:

1. **The algorithm depends on the machine.** FFTW chooses its algorithm by
   timing tests (`FFTW_MEASURE`), and each algorithm, and each CPU's vector
   code, rounds differently.
2. **The timing depends on threads.** The result is used a frame after it was
   requested, and only if the worker has finished.

## The room solver

`src/simulation/gravity/DeterministicGravity.cpp` does the same physics with
these properties:

- **Synchronous:** it runs at the same point of every tick, on the simulation
  thread. It has no state: the same mass and walls always give the same
  forces. An unchanged input is skipped, and that skip is only a shortcut.
- **Own FFT:** a fixed radix-2 FFT on a 512×256 grid. That's more than twice
  the 153×96 cell grid in each direction, so a mass's pull never wraps around
  (the game's solver wraps slightly).
  - One complex transform carries both force components: the kernel spectrum
    is precomputed as KX + iKY.
  - Per tick: one forward and one inverse 2D transform, skipping the rows known
    to be zero.
- **No platform maths library:**
  - Twiddle factors come from a Taylor series in double precision, using only
    the first octant plus symmetry.
  - The kernel `-G d / |d|^3` uses `sqrt`, which IEEE requires to be correctly
    rounded on every platform.
- **Strict IEEE arithmetic in that one file,** whatever the build flags (the
  engine itself is built with `-ffast-math` or `/fp:fast`):
  - clang: `#pragma float_control(precise, on)` and `#pragma clang fp contract(off)`;
  - GCC: `#pragma GCC optimize("no-fast-math", "fp-contract=off")`;
  - MSVC: `float_control(precise, on)` and `fp_contract(off)`.

  Flush-to-zero and denormals-are-zero come from `MatchGameFloatingPoint()`,
  the same as for all room physics.
- **No fused multiply-add, even on ARM.** The pragmas aren't enough here:
  clang's `-ffp-contract=fast`, part of `-ffast-math`, ignores them, and
  ARM64 has fused multiply-add. The first CI run on Apple Silicon produced a
  different digest. Every product that is then added now goes through
  `Round()`, an empty `asm` statement that makes the compiler hold the rounded
  product. It emits no instructions.
  - This was checked by compiling on x86 with FMA enabled and the pragmas
    removed. Without the barrier the digest changes; with it the pinned digest
    is reproduced.
  - CI then confirmed the same digest on GCC, MSVC and Apple clang, on x86-64
    and ARM64. A solve takes about 4.8 ms.
- **Cost:** about 1.4–1.6 ms per tick while the mass changes, on the
  development machine (3.6–3.9 ms before the column pass ran across whole
  rows; see `PERFORMANCE_2026-11-02.md`). Ticks where the mass is unchanged
  cost nothing.

`Simulation::deterministicGravity` (set by every multiplayer `World`) makes
`EnableNewtonianGravity` create this solver. `Gravity` gained a `deterministic`
flag, and `Exchange` and the deleter dispatch on it, in both `Fft.cpp` and
`Null.cpp`. Single-player is unchanged.

## Rooms

- **Setting:** `settings.newtonianGravity` is a normal admin setting. The
  Options checkbox, the quick option and the N key all send it.
- **Saves:** a save with Newtonian gravity loads into a room with it on.
- **Snapshots:** the exact snapshot records whether gravity is on. The
  mass and force maps were already part of it. On restore the solver is
  attached without recomputing, so the restored forces are exactly the
  server's.
- **Fingerprint:** it already covered both the maps and whether gravity is
  on.

## Tests

- `multiplayer-core`:
  - forces point towards mass, and gravity-wall cells have no field;
  - **a pinned digest of every force for a fixed input.** Each CI platform
    (GCC, MSVC, Apple clang on x86-64 and ARM) must reproduce it bit for bit;
    MinGW on Windows was checked locally;
  - an unchanged input is skipped;
  - a snapshot of a room with gravity restores exactly, and two copies stay
    identical for 60 more ticks.
- `multiplayer-checkpoint`: room gravity round-trips, and the game's threaded
  solver is still refused.
- `multiplayer-gravity-room`: an admin turns gravity on and fills the room with
  singularity, dust, gas, fire, stone and a gravity-wall box. Two players verify
  14 fingerprints each without a resync.
- `multiplayer-game-room`: the real game turns it on with the N key.
