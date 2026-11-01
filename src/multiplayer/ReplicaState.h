#pragma once
#include "CheckpointWire.h"
#include "World.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace multiplayer
{
// Thrown when the local state no longer matches the authoritative fingerprint.
// Carries this replica's per-subsystem digests so the server can name the first
// differing groups. The local world must be discarded after this is thrown.
struct ReplicaDesync : std::runtime_error
{
    uint64_t tick, revision;
    Json::Value localFields;
    ReplicaDesync(uint64_t tick, uint64_t revision, Json::Value fields);
};
// Strictly ordered, single-threaded consumer of replica WebSocket messages.
// Renderers observe World only after OnJson/OnBinary/Pump has returned. A server
// seal is the sole tick authority: the world never advances past the latest seal.
//
// By default every command and seal is applied inside OnJson. In paced mode they
// queue instead, and Pump(maxTicks) applies them in wire order while running at
// most maxTicks physics steps. That lets a client spread a batched seal over
// several frames instead of jumping ahead in one.
class ReplicaState
{
    CheckpointAssembler incoming;
    std::string receivedCheckpoint;
    uint64_t checkpointId = 0, checkpointTick = 0, checkpointRevision = 0;
    uint64_t checkpointReset = 0, previousEventId = 0;
    bool checkpointPaused = false, checkpointPending = false;
    bool welcomed = false, ready = false, paced = false;
    uint64_t seals = 0, edits = 0, checks = 0, sealedTick = 0;
    std::deque<Json::Value> queued;
    void Commit(const Json::Value &message);
    void Seal(const Json::Value &message);
public:
    World world;
    // Called after each command is applied to the world, with its commit message.
    std::function<void(const Json::Value &)> committed;
    void SetPaced(bool enabled) { paced = enabled; }
    void OnJson(const Json::Value &message);
    void OnBinary(std::string_view packet);
    // Applies queued messages; returns the number of physics steps run.
    uint64_t Pump(uint64_t maxTicks);
    // Physics ticks the server has sealed that this world has not yet run.
    uint64_t Backlog() const { return ready && sealedTick > world.tick ? sealedTick-world.tick : 0; }
    bool Ready() const { return ready; }
    uint64_t Seals() const { return seals; }
    uint64_t Edits() const { return edits; }
    uint64_t Checks() const { return checks; }
};
}
