#pragma once
#include "Protocol.h"
#include "client/GameSave.h"

namespace multiplayer
{
// A settings command, as every machine applies it.
void ApplySettings(Simulation &sim, const SimulationSettingsChange &settings);
// The stickman a room player steers (slot 1-4), if it exists.
playerst *RoomStickman(Simulation &sim, int slot);
class World
{
public:
    std::unique_ptr<Simulation> sim;
    uint64_t tick = 0, revision = 0, resetGeneration = 0;
    bool paused = false;
    World();
    void Advance();
    void Apply(const Command &command);
    void Load(const std::string &path);
    void Save(const std::string &path) const;
    std::unique_ptr<GameSave> CaptureSave() const;
};
}
