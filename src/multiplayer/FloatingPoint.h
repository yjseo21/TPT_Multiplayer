#pragma once

namespace multiplayer
{
// The game runs physics with x86 denormals flushed to zero (X86_KILL_DENORMALS,
// see X86KillDenormals.cpp). The flag is per thread and changes results, so every
// room participant must step with exactly the same mode: World::Advance and
// World::Apply call this. It is a no-op where the game does not kill denormals.
void MatchGameFloatingPoint();
}
