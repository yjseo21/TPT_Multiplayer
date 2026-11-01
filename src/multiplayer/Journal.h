#pragma once
#include "TickTimeline.h"
#include <json/json.h>
#include <cstdint>
#include <fstream>
#include <string>

namespace multiplayer
{
class World;
struct Command;

// Append-only JSONL diagnostic journal, intended for fixed-seed empty worlds.
// NOT an exact-checkpoint format; the server must be started with --seed and
// must not load/recover a CPS world. No room credentials are persisted.
class Journal
{
    std::ofstream file;
    uint64_t lastCheckedTick = 0;
    void Write(const Json::Value &record);
public:
    Journal(const std::string &path, uint32_t seed, unsigned tickRate, const World &initial);
    void Edit(const EditStamp &stamp, uint32_t actor, const Command &command);
    void Check(const World &world, bool force = false);
};
}
