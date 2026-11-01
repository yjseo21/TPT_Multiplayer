#pragma once
#include <cstdint>
#include <string_view>

namespace multiplayer
{
// Legacy OPS saves may omit RNG state. All replicas loading identical save
// bytes must start from an identical RNG state before Simulation::Load.
// FNV-1a is for deterministic seeding, NOT cryptographic authentication.
inline uint32_t SeedForLegacySave(std::string_view bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char ch : bytes)
        hash = (hash ^ ch) * UINT64_C(1099511628211);
    // RNG::seed accepts 32 bits. Fold both halves so high bits participate.
    return uint32_t(hash) ^ uint32_t(hash >> 32);
}
}
