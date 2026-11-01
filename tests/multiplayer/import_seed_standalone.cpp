// Release builds set NDEBUG (b_ndebug=true); these checks must always run.
#undef NDEBUG
#include "multiplayer/ImportSeed.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
int main()
{
    using multiplayer::SeedForLegacySave;
    const std::string save = std::string("OPS\0RNG",7);
    const auto seed = SeedForLegacySave(std::string_view(save));
    assert(seed == SeedForLegacySave(std::string_view(save)));
    assert(seed != SeedForLegacySave(std::string_view(save).substr(0,6)));
    assert(seed != SeedForLegacySave("other save"));
    assert(SeedForLegacySave(std::string_view("\xff\x80",2)) ==
           SeedForLegacySave(std::string_view("\xff\x80",2)));
    std::cout << "legacy save deterministic seed PASS " << seed << "\n";
}
