#pragma once

namespace multiplayer
{
// Rooms run physics with denormals flushed to zero on every CPU (the game itself
// only does so on some x86 builds). The flag is per thread and changes results, so every
// room participant must step with exactly the same mode: World::Advance and
// World::Apply call this.
void MatchGameFloatingPoint();
}
